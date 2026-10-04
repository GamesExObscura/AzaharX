// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <cstring>
#include "common/archives.h"
#include "common/hacks/hack_manager.h"
#include "common/microprofile.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/loader/loader.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/gpu_impl.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_software/sw_blitter.h"
#include "video_core/right_eye_disabler.h"
#include "video_core/shader/generator/glsl_shader_gen.h"
#include "video_core/video_core.h"

namespace VideoCore {

constexpr VAddr VADDR_LCD = 0x1ED02000;
constexpr VAddr VADDR_GPU = 0x1EF00000;

class DelayGenerator {
private:
    DelayGenerator() = default;

    // Average transfer speed based on measurements taken from real
    // hardware. 4 different modes have been taken into consideration:
    // RAM -> RAM, RAM -> VRAM, VRAM -> RAM and VRAM -> VRAM.
    // Furthermore, measurements are split into DMA transfers and tex
    // copies. For simplicity, we will assume fills are as fast as
    // texture copies.

    static constexpr double mibps_to_ns_per_byte(double mib_per_sec) {
        return 1'000'000'000.0 / (mib_per_sec * 1024.0 * 1024.0);
    }

    static constexpr std::array<std::array<double, 4>, 2> speed_mibps = {
        {{
             190.0, // DMA RAMTORAM
             310.0, // DMA RAMTOVRAM
             380.0, // DMA VRAMTORAM
             380.0, // DMA VRAMTOVRAM
         },
         {
             450.0,  // TEX RAMTORAM
             3100.0, // TEX RAMTOVRAM
             5400.0, // TEX VRAMTORAM
             5400.0, // TEX VRAMTOVRAM
         }}};

public:
    enum class CopyMode {
        RAMTORAM,
        RAMTOVRAM,
        VRAMTORAM,
        VRAMTOVRAM,
    };

    static CopyMode GetCopyMode(bool input_vram, bool output_vram) {
        if (!input_vram && !output_vram) {
            return CopyMode::RAMTORAM;
        } else if (!input_vram && output_vram) {
            return CopyMode::RAMTOVRAM;
        } else if (input_vram && !output_vram) {
            return CopyMode::VRAMTORAM;
        } else {
            return CopyMode::VRAMTOVRAM;
        }
    }

    static u64 CalculateDelayNanoseconds(CopyMode mode, bool is_textre, size_t size) {
        double base_ns_per_byte =
            mibps_to_ns_per_byte(speed_mibps[is_textre][static_cast<u32>(mode)]);

        return static_cast<u64>(size * base_ns_per_byte);
    }
};

MICROPROFILE_DEFINE(GPU_DisplayTransfer, "GPU", "DisplayTransfer", MP_RGB(100, 100, 255));
MICROPROFILE_DEFINE(GPU_CmdlistProcessing, "GPU", "Cmdlist Processing", MP_RGB(100, 255, 100));

GPU::GPU(Core::System& system, Frontend::EmuWindow& emu_window,
         Frontend::EmuWindow* secondary_window)
    : right_eye_disabler{std::make_unique<RightEyeDisabler>(*this)},
      impl{std::make_unique<Impl>(system, emu_window, secondary_window)} {
    impl->vblank_event = impl->timing.RegisterEvent(
        "GPU::VBlankCallback",
        [this](uintptr_t user_data, s64 cycles_late) { VBlankCallback(user_data, cycles_late); });
    impl->timing.ScheduleEvent(FRAME_TICKS, impl->vblank_event);

    // Delayed delivery of GX completion interrupts — the timing event
    // that fires the real GSP handler a sliver of emulated time after
    // the command completed. See HackType::GX_COMPLETION_IRQ_DELAY.
    impl->completion_irq_event = impl->timing.RegisterEvent(
        "GPU::DelayedCompletionIRQ", [this](uintptr_t user_data, s64 cycles_late) {
            if (impl->raw_interrupt_handler) {
                // Our deferral already elapsed via this event; deliver with
                // no additional downstream delay.
                impl->raw_interrupt_handler(
                    static_cast<Service::GSP::InterruptId>(user_data), 0);
            }
        });

    // Bind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);
}

GPU::~GPU() = default;

PAddr GPU::VirtualToPhysicalAddress(VAddr addr) {
    if (addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END) {
        return addr - Memory::VRAM_VADDR + Memory::VRAM_PADDR;
    }
    if (addr >= Memory::LINEAR_HEAP_VADDR && addr <= Memory::LINEAR_HEAP_VADDR_END) {
        return addr - Memory::LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    if (addr >= Memory::NEW_LINEAR_HEAP_VADDR && addr <= Memory::NEW_LINEAR_HEAP_VADDR_END) {
        return addr - Memory::NEW_LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    PAddr plg_fb_addr;
    if (addr >= Memory::PLUGIN_3GX_FB_VADDR && addr <= Memory::PLUGIN_3GX_FB_VADDR_END &&
        (plg_fb_addr = impl->system.Memory().Plugin3GXFramebufferAddress())) {
        return addr - Memory::PLUGIN_3GX_FB_VADDR + plg_fb_addr;
    }

    LOG_ERROR(HW_Memory, "Unknown virtual address @ 0x{:08X}", addr);
    return addr;
}

void GPU::SetInterruptHandler(Service::GSP::InterruptHandler handler) {
    impl->raw_interrupt_handler = handler;
    // GX completion-IRQ delay (HackType::GX_COMPLETION_IRQ_DELAY).
    // On real hardware a GX command takes physical time, so its
    // completion interrupt always arrives AFTER the game's submit path
    // has finished its post-submit bookkeeping. Azahar executes GX
    // commands synchronously inside the Trigger IPC, so the completion
    // fires BEFORE the game resumes — inverting the order. MFA's
    // software command pump halts itself after submitting a
    // stop-flagged entry and relies on that entry's completion
    // interrupt to clear the halt; with inverted order the clear runs
    // first, the halt is set forever, and the asset loader dies
    // mid-burst (missing arms / dress / wardrobe textures). Deferring
    // completion interrupts (DMA/P3D/PPF/PSC0/PSC1 — NOT the PDC
    // vblanks) by ~100us of emulated time restores the hardware
    // ordering. This single wrap point covers every signal site,
    // including P3D from the PICA command processor.
    // Upstream's handler signature now carries a native per-signal delay
    // (nanoseconds, from DelayGenerator). When our hack is FORCE'd we add
    // the 100us deferral ON TOP of the native delay; otherwise pass
    // through untouched.
    Service::GSP::InterruptHandler wrapped = [this](Service::GSP::InterruptId id, u64 delay_ns) {
        using Service::GSP::InterruptId;
        const bool is_completion = id == InterruptId::DMA || id == InterruptId::P3D ||
                                   id == InterruptId::PPF || id == InterruptId::PSC0 ||
                                   id == InterruptId::PSC1;
        if (is_completion && impl->delay_completion_irq) {
            impl->timing.ScheduleEvent(usToCycles(100 + delay_ns / 1000),
                                       impl->completion_irq_event, static_cast<u64>(id));
        } else {
            impl->raw_interrupt_handler(id, delay_ns);
        }
    };
    impl->signal_interrupt = wrapped;
    impl->pica.SetInterruptHandler(wrapped);
}

void GPU::FlushRegion(PAddr addr, u32 size) {
    impl->rasterizer->FlushRegion(addr, size);
}

void GPU::InvalidateRegion(PAddr addr, u32 size) {
    impl->rasterizer->InvalidateRegion(addr, size);
}

void GPU::ClearAll(bool flush) {
    impl->rasterizer->ClearAll(flush);
}

void GPU::Execute(const Service::GSP::Command& command) {
    using Service::GSP::CommandId;
    auto& regs = impl->pica.regs;

    switch (command.id) {
    case CommandId::RequestDma: {
        // TODO(Subv): These memory accesses should not go through the application's memory mapping.
        // They should go through the GSP module's memory mapping.
        const auto process = impl->system.Kernel().GetCurrentProcess();
        impl->memory.CopyBlock(*process, command.dma_request.dest_address,
                               command.dma_request.source_address, command.dma_request.size);

        auto is_vram = [&](u32 addr) {
            return addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END;
        };

        u64 delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(is_vram(command.dma_request.source_address),
                                        is_vram(command.dma_request.dest_address)),
            false, command.dma_request.size);

        impl->signal_interrupt(Service::GSP::InterruptId::DMA, delay);
        break;
    }
    case CommandId::SubmitCmdList: {
        auto& params = command.submit_gpu_cmdlist;
        auto& cmdbuffer = regs.internal.pipeline.command_buffer;

        // Write to the command buffer GPU registers
        cmdbuffer.addr[0].Assign(VirtualToPhysicalAddress(params.address) >> 3);
        cmdbuffer.size[0].Assign(params.size >> 3);
        cmdbuffer.trigger[0] = 1;

        // Trigger processing of the command list
        SubmitCmdList(0);
        break;
    }
    case CommandId::MemoryFill: {
        auto& params = command.memory_fill;
        auto& memfill = regs.memory_fill_config;

        // Write to the memory fill GPU registers.
        // If both buffers are set GSP dispatches PSC0 only.
        //
        // Title-gated fix (bisect-confirmed culprit
        // 0453e4463 "GSP: correct MemoryFill interrupt signaling"): DI's
        // Mobiclip AV pump fills BOTH buffers per video frame and waits
        // for BOTH PSC0 and PSC1. The single-PSC0 dispatch strands its
        // pump in a livelock (1 frame per ~2s, black screen after the
        // intros). Restore the pre-#1218 per-buffer signaling for DI.
        const bool per_buffer_irq_compat =
            Common::Hacks::g_memoryfill_per_buffer_irq.load(std::memory_order_relaxed);
        const bool has_both_bufs =
            params.start1 != 0 && params.start2 != 0 && !per_buffer_irq_compat;
        if (params.start1 != 0) {
            memfill[0].address_start = VirtualToPhysicalAddress(params.start1) >> 3;
            memfill[0].address_end = VirtualToPhysicalAddress(params.end1) >> 3;
            memfill[0].value_32bit = params.value1;
            memfill[0].control = params.control1;
            MemoryFill(0, has_both_bufs ? std::numeric_limits<u32>::max() : 0);
        }
        if (params.start2 != 0) {
            memfill[1].address_start = VirtualToPhysicalAddress(params.start2) >> 3;
            memfill[1].address_end = VirtualToPhysicalAddress(params.end2) >> 3;
            memfill[1].value_32bit = params.value2;
            memfill[1].control = params.control2;
            MemoryFill(1, has_both_bufs ? 0 : 1);
        }
        break;
    }
    case CommandId::DisplayTransfer: {
        auto& params = command.display_transfer;
        auto& display_transfer = regs.display_transfer_config;

        // FB_CONFIG_DIAG (log only): who writes the screens' buffers?
        if (Common::Hacks::g_fb_config_diag.load(std::memory_order_relaxed)) {
            static u32 dt_tick = 0;
            if ((dt_tick++ % 60) == 0) {
                LOG_INFO(HW_GPU, "[FBXFER] DisplayTransfer in=0x{:08X} -> out=0x{:08X} flags=0x{:08X}",
                         VirtualToPhysicalAddress(params.in_buffer_address),
                         VirtualToPhysicalAddress(params.out_buffer_address), params.flags);
            }
        }

        // Write to the transfer engine GPU registers.
        display_transfer.input_address = VirtualToPhysicalAddress(params.in_buffer_address) >> 3;
        display_transfer.output_address = VirtualToPhysicalAddress(params.out_buffer_address) >> 3;
        display_transfer.input_size = params.in_buffer_size;
        display_transfer.output_size = params.out_buffer_size;
        display_transfer.flags = params.flags;
        display_transfer.trigger.Assign(1);

        // Trigger the display transfer.
        MemoryTransfer();
        break;
    }
    case CommandId::TextureCopy: {
        auto& params = command.texture_copy;
        auto& texture_copy = regs.display_transfer_config;

        // FB_CONFIG_DIAG (log only).
        if (Common::Hacks::g_fb_config_diag.load(std::memory_order_relaxed)) {
            static u32 tc_tick = 0;
            if ((tc_tick++ % 60) == 0) {
                LOG_INFO(HW_GPU, "[FBXFER] TextureCopy in=0x{:08X} -> out=0x{:08X} size=0x{:X}",
                         VirtualToPhysicalAddress(params.in_buffer_address),
                         VirtualToPhysicalAddress(params.out_buffer_address), params.size);
            }
        }

        // Write to the transfer engine GPU registers.
        texture_copy.input_address = VirtualToPhysicalAddress(params.in_buffer_address) >> 3;
        texture_copy.output_address = VirtualToPhysicalAddress(params.out_buffer_address) >> 3;
        texture_copy.texture_copy.size = params.size;
        texture_copy.texture_copy.input_size = params.in_width_gap;
        texture_copy.texture_copy.output_size = params.out_width_gap;
        texture_copy.flags = params.flags;
        texture_copy.trigger.Assign(1);

        // Trigger the texture copy.
        MemoryTransfer();
        break;
    }
    case CommandId::CacheFlush: {
        // Rasterizer flushing handled elsewhere in CPU read/write and other GPU handlers
        // Use command.cache_flush.regions to implement this handler
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Unknown command {:#08X}", command.id.Value());
    }

    // Notify debugger that a GSP command was processed.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::GSPCommandProcessed, &command);
    }
}

void GPU::SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info) {
    const PAddr phys_address_left = VirtualToPhysicalAddress(info.address_left);
    const PAddr phys_address_right = VirtualToPhysicalAddress(info.address_right);

    // Update framebuffer properties.
    auto& framebuffer = impl->pica.regs.framebuffer_config[screen_id];
    if (info.active_fb == 0) {
        framebuffer.address_left1 = phys_address_left;
        framebuffer.address_right1 = phys_address_right;
    } else {
        framebuffer.address_left2 = phys_address_left;
        framebuffer.address_right2 = phys_address_right;
    }

    framebuffer.stride = info.stride;
    framebuffer.format = info.format;
    framebuffer.active_fb = info.shown_fb;

    // Notify debugger about the buffer swap.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::BufferSwapped, nullptr);
    }

    if (screen_id == 0) {
        MicroProfileFlip();
        impl->system.perf_stats->EndGameFrame();
        right_eye_disabler->ReportEndFrame();
    }
}

void GPU::SetColorFill(const Pica::ColorFill& fill) {
    impl->pica.regs_lcd.color_fill_top = fill;
    impl->pica.regs_lcd.color_fill_bottom = fill;
}

u32 GPU::ReadReg(VAddr addr) {
    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        return impl->pica.regs_lcd[index];
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        return impl->pica.regs.reg_array[index];
    }
    default:
        UNREACHABLE_MSG("Read from unknown GPU address {:#08X}", addr);
    }
}

void GPU::WriteReg(VAddr addr, u32 data) {
    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        impl->pica.regs_lcd[index] = data;
        break;
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);

        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        impl->pica.regs.reg_array[index] = data;

        // Handle registers that trigger GPU actions
        switch (index) {
        case GPU_REG_INDEX(memory_fill_config[0].trigger):
            MemoryFill(0, 0);
            break;
        case GPU_REG_INDEX(memory_fill_config[1].trigger):
            MemoryFill(1, 1);
            break;
        case GPU_REG_INDEX(display_transfer_config.trigger):
            MemoryTransfer();
            break;
        case GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[0]):
            SubmitCmdList(0);
            break;
        case GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[1]):
            SubmitCmdList(1);
            break;
        default:
            break;
        }
        break;
    }
    default:
        UNREACHABLE_MSG("Write to unknown GPU address {:#08X}", addr);
    }
}

VideoCore::RendererBase& GPU::Renderer() {
    return *impl->renderer;
}

Pica::PicaCore& GPU::PicaCore() {
    return impl->pica;
}

const Pica::PicaCore& GPU::PicaCore() const {
    return impl->pica;
}

GraphicsDebugger& GPU::Debugger() {
    return impl->gpu_debugger;
}

void GPU::ApplyPerProgramSettings(u64 program_ID) {
    auto hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::ACCURATE_MULTIPLICATION, program_ID);
    bool use_accurate_mul = Settings::values.shaders_accurate_mul.GetValue();
    if (hack) {
        switch (hack->mode) {
        case Common::Hacks::HackAllowMode::DISALLOW:
            use_accurate_mul = false;
            break;
        case Common::Hacks::HackAllowMode::FORCE:
            use_accurate_mul = true;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    impl->rasterizer->SetAccurateMul(use_accurate_mul);

    // Per-title vertex-color normalization. Default off (preserves existing
    // Citra behavior for every other game); FORCE for Disney Princess: MFA,
    // whose VS expects 0..1 floats from a u8 vertex color attribute.
    auto norm_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::NORMALIZE_VERTEX_COLORS, program_ID);
    bool normalize_vertex_colors = false;
    if (norm_hack) {
        switch (norm_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            normalize_vertex_colors = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            normalize_vertex_colors = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    impl->rasterizer->SetNormalizeVertexColors(normalize_vertex_colors);

    // Per-title mid-gray ETC1 fallback for Disney Princess MFA. See HackType
    // enum comment for the full rationale.
    auto midgray_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::MID_GRAY_ETC1_FALLBACK, program_ID);
    bool mid_gray_etc1 = false;
    if (midgray_hack) {
        switch (midgray_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            mid_gray_etc1 = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            mid_gray_etc1 = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    impl->rasterizer->SetMidGrayEtc1Fallback(mid_gray_etc1);

    // PMG3D Zeusiota-style fix — force tex0 sampling at base mip
    // (textureLod = 0) in the generated FS. Title-gated via
    // HackType::PMG3D_FORCE_WHITE_AMBIENT (legacy enum name; actual
    // effect is LOD-0 sampling).
    auto pmg3d_lod0_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::PMG3D_FORCE_WHITE_AMBIENT, program_ID);
    bool pmg3d_force_lod0 = false;
    if (pmg3d_lod0_hack) {
        switch (pmg3d_lod0_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            pmg3d_force_lod0 = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            pmg3d_force_lod0 = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Pica::Shader::Generator::GLSL::EnablePmg3dForceLod0(pmg3d_force_lod0);

    // PMG3D Zeusiota-style fix — relax the PICA z<=0 clip plane in all
    // generated VS/GS so logos/UI/gameplay survive clipping. Side
    // effect: 3D building + host character don't render. Title-gated
    // via HackType::PMG3D_RELAX_CLIP_PLANE.
    auto pmg3d_relax_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::PMG3D_RELAX_CLIP_PLANE, program_ID);
    bool pmg3d_relax_clip = false;
    if (pmg3d_relax_hack) {
        switch (pmg3d_relax_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            pmg3d_relax_clip = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            pmg3d_relax_clip = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Pica::Shader::Generator::GLSL::EnablePmg3dRelaxClipPlane(pmg3d_relax_clip);

    // Accurate JIT memory — see HackType::ACCURATE_JIT_MEMORY. This
    // call happens during title load, before the app process's page
    // table (and therefore its dynarmic JIT) is created, so the flag
    // is guaranteed to be observed by ARM_Dynarmic::MakeJit.
    auto accurate_mem_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::ACCURATE_JIT_MEMORY, program_ID);
    bool accurate_jit_memory = false;
    if (accurate_mem_hack) {
        switch (accurate_mem_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            accurate_jit_memory = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            accurate_jit_memory = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_accurate_jit_memory.store(accurate_jit_memory,
                                               std::memory_order_relaxed);
    if (accurate_jit_memory) {
        LOG_INFO(HW_GPU,
                 "[ACCURATE-MEM] JIT page-table fast path disabled for program 0x{:016x}",
                 program_ID);
    }

    // GX completion-IRQ delay — see HackType::GX_COMPLETION_IRQ_DELAY.
    auto irq_delay_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::GX_COMPLETION_IRQ_DELAY, program_ID);
    bool delay_completion_irq = false;
    if (irq_delay_hack) {
        switch (irq_delay_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            delay_completion_irq = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            delay_completion_irq = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    impl->delay_completion_irq = delay_completion_irq;
    if (delay_completion_irq) {
        LOG_INFO(HW_GPU,
                 "[GX-IRQ-DELAY] deferred completion interrupts enabled for program 0x{:016x}",
                 program_ID);
    }

    // Graceful JIT fallback — see HackType::GRACEFUL_JIT_FALLBACK.
    auto graceful_fallback_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::GRACEFUL_JIT_FALLBACK, program_ID);
    bool graceful_jit_fallback = false;
    if (graceful_fallback_hack) {
        switch (graceful_fallback_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            graceful_jit_fallback = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            graceful_jit_fallback = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_graceful_jit_fallback.store(graceful_jit_fallback,
                                                 std::memory_order_relaxed);
    if (graceful_jit_fallback) {
        LOG_INFO(HW_GPU,
                 "[GRACEFUL-JIT] interpreter fallback enabled for program 0x{:016x}",
                 program_ID);
    }

    // Overbright vertex colors — see HackType::OVERBRIGHT_VERTEX_COLORS.
    auto overbright_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::OVERBRIGHT_VERTEX_COLORS, program_ID);
    bool overbright_vertex_colors = false;
    if (overbright_hack) {
        switch (overbright_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            overbright_vertex_colors = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            overbright_vertex_colors = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_overbright_vertex_colors.store(overbright_vertex_colors,
                                                    std::memory_order_relaxed);
    if (overbright_vertex_colors) {
        LOG_INFO(HW_GPU,
                 "[OVERBRIGHT-VTX] vertex-color clamp raised to 2.0 for program 0x{:016x}",
                 program_ID);
    }

    // FROGGER_SPECULAR_BLUE (title-gated) — see HackType::FROGGER_SPECULAR_BLUE.
    auto frogblue_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FROGGER_SPECULAR_BLUE, program_ID);
    bool frogger_specular_blue = false;
    if (frogblue_hack && frogblue_hack->mode == Common::Hacks::HackAllowMode::FORCE) {
        frogger_specular_blue = true;
    }
    Common::Hacks::g_frogger_specular_blue.store(frogger_specular_blue,
                                                 std::memory_order_relaxed);
    if (frogger_specular_blue) {
        LOG_INFO(HW_GPU,
                 "[SPEC-BLUE] TEV blue-channel reads of secondary_fragment_color substituted "
                 "for program 0x{:016x}",
                 program_ID);
    }

    // Right-eye display fallback — see HackType::RIGHT_EYE_DISPLAY_FALLBACK.
    auto fb_fallback_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::RIGHT_EYE_DISPLAY_FALLBACK, program_ID);
    bool right_eye_display_fallback = false;
    if (fb_fallback_hack) {
        switch (fb_fallback_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            right_eye_display_fallback = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            right_eye_display_fallback = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_right_eye_display_fallback.store(right_eye_display_fallback,
                                                      std::memory_order_relaxed);
    if (right_eye_display_fallback) {
        LOG_INFO(HW_GPU,
                 "[FB-FALLBACK] right-eye display fallback enabled for program 0x{:016x}",
                 program_ID);
    }

    // Per-buffer MemoryFill interrupts — see HackType::MEMORYFILL_PER_BUFFER_IRQ.
    auto memfill_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::MEMORYFILL_PER_BUFFER_IRQ, program_ID);
    bool memoryfill_per_buffer_irq = false;
    if (memfill_hack) {
        switch (memfill_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            memoryfill_per_buffer_irq = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            memoryfill_per_buffer_irq = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_memoryfill_per_buffer_irq.store(memoryfill_per_buffer_irq,
                                                     std::memory_order_relaxed);
    if (memoryfill_per_buffer_irq) {
        LOG_INFO(HW_GPU,
                 "[MEMFILL-COMPAT] per-buffer PSC0/PSC1 signaling enabled for program 0x{:016x}",
                 program_ID);
    }

    // Keep last frame on zero framebuffer address — see
    // HackType::KEEP_LAST_FRAME_ON_ZERO_FB.
    auto zero_fb_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::KEEP_LAST_FRAME_ON_ZERO_FB, program_ID);
    bool keep_last_frame_on_zero_fb = false;
    if (zero_fb_hack) {
        switch (zero_fb_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            keep_last_frame_on_zero_fb = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            keep_last_frame_on_zero_fb = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_keep_last_frame_on_zero_fb.store(keep_last_frame_on_zero_fb,
                                                      std::memory_order_relaxed);
    if (keep_last_frame_on_zero_fb) {
        LOG_INFO(HW_GPU,
                 "[ZERO-FB-KEEP] keep-last-frame on zero fb address enabled for program 0x{:016x}",
                 program_ID);
    }

    // Skip core-1 preemption — see HackType::SKIP_CORE1_PREEMPTION.
    auto core1_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::SKIP_CORE1_PREEMPTION, program_ID);
    bool skip_core1_preemption = false;
    if (core1_hack) {
        switch (core1_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            skip_core1_preemption = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            skip_core1_preemption = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_skip_core1_preemption.store(skip_core1_preemption,
                                                 std::memory_order_relaxed);
    if (skip_core1_preemption) {
        LOG_INFO(HW_GPU, "[CORE1-SKIP] core-1 preemption disabled for program 0x{:016x}",
                 program_ID);
    }

    // Zero-fill FS ControlArchive output — see HackType::FS_CONTROL_ZERO_OUTPUT.
    auto fs_control_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FS_CONTROL_ZERO_OUTPUT, program_ID);
    bool fs_control_zero_output = false;
    if (fs_control_hack) {
        switch (fs_control_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            fs_control_zero_output = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            fs_control_zero_output = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_fs_control_zero_output.store(fs_control_zero_output,
                                                  std::memory_order_relaxed);
    if (fs_control_zero_output) {
        LOG_INFO(HW_GPU, "[FS-CONTROL-ZERO] ControlArchive output zero-fill for program 0x{:016x}",
                 program_ID);
    }

    // One-fill FS ControlArchive output (Torus engine) — see
    // HackType::FS_CONTROL_ONE_OUTPUT.
    auto fs_control_one_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FS_CONTROL_ONE_OUTPUT, program_ID);
    bool fs_control_one_output = false;
    if (fs_control_one_hack) {
        switch (fs_control_one_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            fs_control_one_output = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            fs_control_one_output = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_fs_control_one_output.store(fs_control_one_output,
                                                 std::memory_order_relaxed);
    if (fs_control_one_output) {
        LOG_INFO(HW_GPU, "[FS-CONTROL-ONE] ControlArchive output one-fill for program 0x{:016x}",
                 program_ID);
    }

    // Simulated GPU timings off — see HackType::DISABLE_GPU_TIMING_SIM.
    auto gpu_timing_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::DISABLE_GPU_TIMING_SIM, program_ID);
    bool disable_gpu_timing_sim = false;
    if (gpu_timing_hack) {
        switch (gpu_timing_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            disable_gpu_timing_sim = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            disable_gpu_timing_sim = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_disable_gpu_timing_sim.store(disable_gpu_timing_sim,
                                                  std::memory_order_relaxed);
    if (disable_gpu_timing_sim) {
        LOG_INFO(HW_GPU, "[GPU-TIMING-OFF] immediate GPU completion IRQs for program 0x{:016x}",
                 program_ID);
    }

    // Present every frame — see HackType::FORCE_PRESENT_EVERY_FRAME.
    auto present_all_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FORCE_PRESENT_EVERY_FRAME, program_ID);
    bool force_present_every_frame = false;
    if (present_all_hack && present_all_hack->mode == Common::Hacks::HackAllowMode::FORCE) {
        force_present_every_frame = true;
    }
    Common::Hacks::g_force_present_every_frame.store(force_present_every_frame,
                                                     std::memory_order_relaxed);
    if (force_present_every_frame) {
        LOG_INFO(HW_GPU,
                 "[PRESENT-ALL] presenting every frame (skip-duplicate-frames bypassed) for "
                 "program 0x{:016x}",
                 program_ID);
    }

    // Pre-#69 validation-skip rule — see HackType::LEGACY_VALIDATION_SKIP.
    auto validation_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::LEGACY_VALIDATION_SKIP, program_ID);
    const bool legacy_validation =
        validation_hack && validation_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_legacy_validation_skip.store(legacy_validation, std::memory_order_relaxed);
    if (legacy_validation) {
        LOG_INFO(HW_GPU, "[VALIDATE-LEGACY] pre-#69 validation-skip rule for program 0x{:016x}",
                 program_ID);
    }

    // SaveData free space = formatted size — see HackType::SAVEDATA_FREE_BYTES_FROM_FORMAT.
    auto free_bytes_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::SAVEDATA_FREE_BYTES_FROM_FORMAT, program_ID);
    const bool free_bytes_from_format =
        free_bytes_hack && free_bytes_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_savedata_free_bytes_from_format.store(free_bytes_from_format,
                                                           std::memory_order_relaxed);
    if (free_bytes_from_format) {
        LOG_INFO(HW_GPU, "[SAVE-FREEBYTES] SaveData free space = formatted size for program 0x{:016x}",
                 program_ID);
    }

    // Pending GX commands run on GPU-right acquire — see HackType::GSP_DRAIN_QUEUE_ON_ACQUIRE.
    auto drain_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::GSP_DRAIN_QUEUE_ON_ACQUIRE, program_ID);
    const bool drain_on_acquire =
        drain_hack && drain_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_gsp_drain_queue_on_acquire.store(drain_on_acquire, std::memory_order_relaxed);
    if (drain_on_acquire) {
        LOG_INFO(HW_GPU, "[GXQ-DRAIN] pending GX commands run on GPU-right acquire for program 0x{:016x}",
                 program_ID);
    }

    // Unlink destroyed effect entities — see HackType::JAWS_ENTITY_UNLINK.
    auto unlink_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::JAWS_ENTITY_UNLINK, program_ID);
    const bool entity_unlink =
        unlink_hack && unlink_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_jaws_entity_unlink.store(entity_unlink, std::memory_order_relaxed);
    if (entity_unlink) {
        LOG_INFO(HW_GPU, "[ENTITY-UNLINK] destroyed-entity unlink armed for program 0x{:016x}",
                 program_ID);
    }

    // Async-task wake guard on Stop — see HackType::ASYNC_WAKE_SHUTDOWN_GUARD.
    auto wake_guard_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::ASYNC_WAKE_SHUTDOWN_GUARD, program_ID);
    const bool wake_guard =
        wake_guard_hack && wake_guard_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_async_wake_shutdown_guard.store(wake_guard, std::memory_order_relaxed);
    if (wake_guard) {
        LOG_INFO(HW_GPU, "[ASYNC-GUARD] async-task wake guard on Stop armed for program 0x{:016x}",
                 program_ID);
    }

    // Automatic render-thread delay — see HackType::AUTO_RENDER_THREAD_DELAY.
    // Per-title value: Carnival Games: Wild West 3D only needs the thread to yield after each
    // submit (1 us: 29 fps at 100% speed; 5.77 ms cut it to 14-22 fps). Captain America moves
    // its characters per frame for a 30 fps game: at 60 fps (no delay) they run and jump too
    // far, below 30 they move in slow motion. It submits ~13 lists a frame, so its fps scales
    // with the delay (5.77 ms: 12-14, 2.5 ms: 28-29, 2.25 ms: 28-31, 2.0 ms: 34-35, 0: 60).
    // WWE uses the 5.77 ms the user runs it with.
    const u32 auto_render_delay_us = program_ID == 0x0004000000061400ULL   ? 1
                                     : program_ID == 0x0004000000040600ULL ? 2250
                                                                           : 5770;
    auto render_delay_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::AUTO_RENDER_THREAD_DELAY, program_ID);
    const bool render_delay =
        render_delay_hack && render_delay_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_auto_render_thread_delay_us.store(render_delay ? auto_render_delay_us : 0,
                                                       std::memory_order_relaxed);
    if (render_delay) {
        LOG_INFO(HW_GPU,
                 "[RENDER-DELAY] automatic render-thread delay {} us for program 0x{:016x} "
                 "(user setting {} us wins if non-zero)",
                 auto_render_delay_us, program_ID,
                 Settings::values.delay_game_render_thread_us.GetValue());
    }

    // Per-draw lookup reuse — see HackType::DRAW_LOOKUP_REUSE.
    auto draw_reuse_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::DRAW_LOOKUP_REUSE, program_ID);
    const bool draw_reuse =
        draw_reuse_hack && draw_reuse_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_draw_lookup_reuse.store(draw_reuse, std::memory_order_relaxed);
    if (draw_reuse) {
        LOG_INFO(HW_GPU, "[DRAW-REUSE] per-draw lookup reuse enabled for program 0x{:016x}",
                 program_ID);
    }

    // NaN float32 uniforms stored as 0 — see HackType::FLOAT_UNIFORM_NAN_TO_ZERO.
    auto uniform_nan_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FLOAT_UNIFORM_NAN_TO_ZERO, program_ID);
    const bool uniform_nan =
        uniform_nan_hack && uniform_nan_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_float_uniform_nan_to_zero.store(uniform_nan, std::memory_order_relaxed);
    if (uniform_nan) {
        LOG_INFO(HW_GPU, "[UNIFORM-NAN] NaN float32 uniforms stored as 0 for program 0x{:016x}",
                 program_ID);
    }

    // Framebuffer clears instead of glClearTexSubImage — see HackType::FORCE_FBO_CLEAR.
    auto fbo_clear_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FORCE_FBO_CLEAR, program_ID);
    const bool force_fbo_clear =
        fbo_clear_hack && fbo_clear_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_force_fbo_clear.store(force_fbo_clear, std::memory_order_relaxed);
    if (force_fbo_clear) {
        LOG_INFO(HW_GPU, "[FBO-CLEAR] texture clears use framebuffer path for program 0x{:016x}",
                 program_ID);
    }

    // Shadow tie counts as lit — see HackType::SHADOW_EQUAL_DEPTH_LIT.
    auto shadow_tie_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::SHADOW_EQUAL_DEPTH_LIT, program_ID);
    const bool shadow_tie_lit =
        shadow_tie_hack && shadow_tie_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_shadow_equal_depth_lit.store(shadow_tie_lit, std::memory_order_relaxed);
    if (shadow_tie_lit) {
        LOG_INFO(HW_GPU, "[SHADOW-TIE-LIT] shadow compare: equal depth is lit for program 0x{:016x}",
                 program_ID);
    }

    // Hardware vertex-color saturate — see HackType::HW_VERTEX_COLOR_SATURATE.
    auto saturate_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::HW_VERTEX_COLOR_SATURATE, program_ID);
    const bool hw_saturate =
        saturate_hack && saturate_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_hw_vertex_color_saturate.store(hw_saturate, std::memory_order_relaxed);
    if (hw_saturate) {
        LOG_INFO(HW_GPU,
                 "[VTXCOLOR-SATURATE] hardware VS vertex colors clamped to [0,1] for program "
                 "0x{:016x}",
                 program_ID);
    }

    // Invalid cull mode keeps previous state — see HackType::CULL_MODE_INVALID_KEEPS_STATE.
    auto cull_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::CULL_MODE_INVALID_KEEPS_STATE, program_ID);
    const bool cull_keep =
        cull_hack && cull_hack->mode == Common::Hacks::HackAllowMode::FORCE;
    Common::Hacks::g_cull_mode_invalid_keeps_state.store(cull_keep, std::memory_order_relaxed);
    if (cull_keep) {
        LOG_INFO(HW_GPU,
                 "[CULL-KEEP] invalid cull mode keeps previous state for program 0x{:016x}",
                 program_ID);
    }

    // Citra-2104 GSP PDC delivery — see HackType::GSP_LEGACY_PDC_QUEUE.
    auto gsp_pdc_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::GSP_LEGACY_PDC_QUEUE, program_ID);
    bool gsp_legacy_pdc_queue = false;
    if (gsp_pdc_hack) {
        switch (gsp_pdc_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            gsp_legacy_pdc_queue = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            gsp_legacy_pdc_queue = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_gsp_legacy_pdc_queue.store(gsp_legacy_pdc_queue, std::memory_order_relaxed);
    if (gsp_legacy_pdc_queue) {
        LOG_INFO(HW_GPU, "[GSP-PDC-LEGACY] unconditional PDC queue+signal for program 0x{:016x}",
                 program_ID);
    }

    // Save-file write diagnostic — see HackType::FS_SAVE_DIAG.
    auto fs_save_diag_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FS_SAVE_DIAG, program_ID);
    bool fs_save_diag = false;
    if (fs_save_diag_hack) {
        switch (fs_save_diag_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            fs_save_diag = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            fs_save_diag = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_fs_save_diag.store(fs_save_diag, std::memory_order_relaxed);
    if (fs_save_diag) {
        LOG_INFO(HW_GPU, "[FS-SAVE] save-write diagnostic armed for program 0x{:016x}", program_ID);
    }

    // Vblank signal-before-present — see HackType::VBLANK_SIGNAL_BEFORE_PRESENT.
    auto vblank_order_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::VBLANK_SIGNAL_BEFORE_PRESENT, program_ID);
    bool vblank_signal_first = false;
    if (vblank_order_hack) {
        switch (vblank_order_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            vblank_signal_first = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            vblank_signal_first = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_vblank_signal_before_present.store(vblank_signal_first,
                                                        std::memory_order_relaxed);
    if (vblank_signal_first) {
        LOG_INFO(HW_GPU, "[VBLANK-ORDER] signal-before-present enabled for program 0x{:016x}",
                 program_ID);
    }

    // Bottom-screen CPU upload — see HackType::FORCE_CPU_BOTTOM_SCREEN.
    auto cpu_bottom_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FORCE_CPU_BOTTOM_SCREEN, program_ID);
    bool force_cpu_bottom_screen = false;
    if (cpu_bottom_hack && cpu_bottom_hack->mode == Common::Hacks::HackAllowMode::FORCE) {
        force_cpu_bottom_screen = true;
    }
    Common::Hacks::g_force_cpu_bottom_screen.store(force_cpu_bottom_screen,
                                                   std::memory_order_relaxed);
    if (force_cpu_bottom_screen) {
        LOG_INFO(HW_GPU, "[CPU-BOTTOM] bottom screen via CPU upload for program 0x{:016x}",
                 program_ID);
    }

    // Flip-rect relocation skip — see HackType::FB_FLIP_RECT_DISABLE.
    auto flip_rect_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FB_FLIP_RECT_DISABLE, program_ID);
    bool fb_flip_rect_disable = false;
    if (flip_rect_hack && flip_rect_hack->mode == Common::Hacks::HackAllowMode::FORCE) {
        fb_flip_rect_disable = true;
    }
    Common::Hacks::g_fb_flip_rect_disable.store(fb_flip_rect_disable, std::memory_order_relaxed);
    if (fb_flip_rect_disable) {
        LOG_INFO(HW_GPU, "[FLIP-RECT] viewport flip relocation disabled for program 0x{:016x}",
                 program_ID);
    }

    // Per-screen framebuffer config probe — see HackType::FB_CONFIG_DIAG.
    auto fb_diag_hack =
        Common::Hacks::hack_manager.GetHack(Common::Hacks::HackType::FB_CONFIG_DIAG, program_ID);
    bool fb_config_diag = false;
    if (fb_diag_hack && fb_diag_hack->mode == Common::Hacks::HackAllowMode::FORCE) {
        fb_config_diag = true;
    }
    Common::Hacks::g_fb_config_diag.store(fb_config_diag, std::memory_order_relaxed);
    if (fb_config_diag) {
        LOG_INFO(HW_GPU, "[FBCFG] framebuffer-config probe enabled for program 0x{:016x}",
                 program_ID);
    }

    // Kernel blocking-wait diagnostic — see HackType::KERNEL_WAIT_DIAG.
    auto kernel_wait_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::KERNEL_WAIT_DIAG, program_ID);
    bool kernel_wait_diag = false;
    if (kernel_wait_hack) {
        switch (kernel_wait_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            kernel_wait_diag = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            kernel_wait_diag = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_kernel_wait_diag.store(kernel_wait_diag, std::memory_order_relaxed);
    if (kernel_wait_diag) {
        LOG_INFO(HW_GPU, "[KWAIT] kernel blocking-wait diagnostic enabled for program 0x{:016x}",
                 program_ID);
    }

    // Zeusiota reference-build hacks — see the HackType comments.
    auto downcount_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::CORE_DOWNCOUNT_HACK, program_ID);
    bool core_downcount = false;
    if (downcount_hack) {
        switch (downcount_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            core_downcount = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            core_downcount = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_core_downcount_hack.store(core_downcount, std::memory_order_relaxed);
    if (core_downcount) {
        LOG_INFO(HW_GPU, "[ZEUS-DOWNCOUNT] core downcount hack enabled for program 0x{:016x}",
                 program_ID);
    }

    auto reinterp_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::IGNORE_FORMAT_REINTERPRETATION, program_ID);
    bool ignore_reinterp = false;
    if (reinterp_hack) {
        switch (reinterp_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            ignore_reinterp = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            ignore_reinterp = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_ignore_format_reinterpretation.store(ignore_reinterp,
                                                          std::memory_order_relaxed);
    if (ignore_reinterp) {
        LOG_INFO(HW_GPU, "[ZEUS-REINTERP] ignore format reinterpretation for program 0x{:016x}",
                 program_ID);
    }

    auto pica_index_hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::FORCE_VALIDATED_PICA_INDEX, program_ID);
    bool force_pica_index = false;
    if (pica_index_hack) {
        switch (pica_index_hack->mode) {
        case Common::Hacks::HackAllowMode::FORCE:
            force_pica_index = true;
            break;
        case Common::Hacks::HackAllowMode::DISALLOW:
            force_pica_index = false;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    Common::Hacks::g_force_validated_pica_index.store(force_pica_index, std::memory_order_relaxed);
    if (force_pica_index) {
        LOG_INFO(HW_GPU, "[ZEUS-PICA] force validated PICA index writes for program 0x{:016x}",
                 program_ID);
    }
}

void GPU::SubmitCmdList(u32 index) {
    // Check if a command list was triggered.
    auto& config = impl->pica.regs.internal.pipeline.command_buffer;
    if (!config.trigger[index]) {
        return;
    }

    MICROPROFILE_SCOPE(GPU_CmdlistProcessing);

    // Forward command list processing to the PICA core.
    const PAddr addr = config.GetPhysicalAddress(index);
    const u32 size = config.GetSize(index);
    impl->pica.ProcessCmdList(addr, size,
                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size));
    config.trigger[index] = 0;
}

void GPU::MemoryFill(u32 index, u32 intr_index) {
    // Check if a memory fill was triggered.
    auto& config = impl->pica.regs.memory_fill_config[index];
    if (!config.trigger) {
        return;
    }

    // Perform memory fill.
    if (!impl->rasterizer->AccelerateFill(config)) {
        impl->sw_blitter->MemoryFill(config);
    }

    // Treat fill as texture transfer from VRAM
    u64 delay = DelayGenerator::CalculateDelayNanoseconds(
        DelayGenerator::GetCopyMode(true, config.IsVRAM()), true,
        config.GetEndAddress() - config.GetStartAddress());

    // It seems that it won't signal interrupt if "address_start" is zero.
    // TODO: hwtest this
    if (config.GetStartAddress() != 0) {
        if (intr_index == 0) {
            impl->signal_interrupt(Service::GSP::InterruptId::PSC0, delay);
        } else if (intr_index == 1) {
            impl->signal_interrupt(Service::GSP::InterruptId::PSC1, delay);
        }
    }

    // Reset "trigger" flag and set the "finish" flag
    // This was confirmed to happen on hardware even if "address_start" is zero.
    config.trigger.Assign(0);
    config.finished.Assign(1);
}

void GPU::MemoryTransfer() {
    // Check if a transfer was triggered.
    auto& config = impl->pica.regs.display_transfer_config;
    if (!config.trigger.Value()) {
        return;
    }

    // Notify debugger about the display transfer.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::IncomingDisplayTransfer, nullptr);
    }

    u64 delay{};
    // Perform memory transfer
    if (config.is_texture_copy) {
        if (!impl->rasterizer->AccelerateTextureCopy(config)) {
            impl->sw_blitter->TextureCopy(config);
        }
        delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
            config.texture_copy.size);
    } else {
        const bool allowed = right_eye_disabler->ShouldAllowDisplayTransfer(
            config.GetPhysicalInputAddress(), config.input_height);
        bool accelerated = false;
        if (allowed) {
            accelerated = impl->rasterizer->AccelerateDisplayTransfer(config);
            if (!accelerated) {
                impl->sw_blitter->DisplayTransfer(config);
            }
        }
        // [XFEROUT] GX command #3 is a VRAM->FCRAM readback the game inspects
        // before deciding whether to build its render-target setup lists. Dump
        // what we actually landed in guest memory so it can be compared byte
        // for byte against Citra 2104, which renders this title.
        {
            static u32 xferout_n = 0;
            if (xferout_n < 8) {
                ++xferout_n;
                const PAddr dst = config.GetPhysicalOutputAddress();
                const u8* p = impl->memory.GetPhysicalPointer(dst);
                u32 sum = 0;
                std::string head;
                if (p) {
                    for (u32 i = 0; i < 1024; ++i) {
                        sum = sum * 31u + p[i];
                    }
                    for (u32 i = 0; i < 16; ++i) {
                        head += fmt::format("{:02X}", p[i]);
                    }
                }
                LOG_CRITICAL(HW_GPU,
                             "[XFEROUT] {} dst={:#010x} accel={} sum1k={:#010x} head={} "
                             "in={}x{} out={}x{} flags={:#x}",
                             xferout_n, dst, accelerated, sum, head, config.input_width.Value(),
                             config.input_height.Value(), config.output_width.Value(),
                             config.output_height.Value(), config.flags);
            }
        }
        // FB_CONFIG_DIAG (log only): did the blit actually happen, and by
        // which path? "allowed=0" means the right-eye disabler dropped it
        // entirely — neither GPU nor CPU wrote the destination.
        if (Common::Hacks::g_fb_config_diag.load(std::memory_order_relaxed)) {
            // Log a BURST of consecutive transfers, not every Nth: a fixed
            // stride aliases against per-frame buffer alternation and would
            // make an alternating L/R pattern look like a single target.
            static u32 xfer_tick = 0;
            if ((xfer_tick++ % 121) < 6) {
                LOG_INFO(HW_GPU,
                         "[FBGUARD] allowed={} accel={} in=0x{:08X} -> out=0x{:08X} "
                         "in={}x{} out={}x{} crop={} flipv={} scal={} ifmt={} ofmt={}",
                         allowed ? 1 : 0, accelerated ? 1 : 0, config.GetPhysicalInputAddress(),
                         config.GetPhysicalOutputAddress(), config.input_width.Value(),
                         config.input_height.Value(), config.output_width.Value(),
                         config.output_height.Value(), config.crop_input_lines ? 1 : 0,
                         config.flip_vertically ? 1 : 0, static_cast<u32>(config.scaling.Value()),
                         static_cast<u32>(config.input_format.Value()),
                         static_cast<u32>(config.output_format.Value()));
            }
        }
        delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
            config.input_width * config.input_height * BytesPerPixel(config.input_format));
    }

    // Complete transfer.
    config.trigger.Assign(0);
    impl->signal_interrupt(Service::GSP::InterruptId::PPF, delay);
}

void GPU::VBlankCallback(std::uintptr_t user_data, s64 cycles_late) {
    // VBLANK_SIGNAL_BEFORE_PRESENT (title-gated, upstream #2273 ordering):
    // signal PDC0/PDC1 BEFORE presenting. Cars 2 stalls at boot forever on
    // the legacy present-first order.
    const bool signal_first =
        Common::Hacks::g_vblank_signal_before_present.load(std::memory_order_relaxed);

    if (signal_first) {
        impl->signal_interrupt(Service::GSP::InterruptId::PDC0, 0);
        impl->signal_interrupt(Service::GSP::InterruptId::PDC1, 0);
    }

    // Present renderered frame.
    impl->renderer->SwapBuffers();

    if (!signal_first) {
        // Signal to GSP that GPU interrupt has occurred
        impl->signal_interrupt(Service::GSP::InterruptId::PDC0, 0);
        impl->signal_interrupt(Service::GSP::InterruptId::PDC1, 0);
    }

    // Reschedule recurrent event
    impl->timing.ScheduleEvent(FRAME_TICKS - cycles_late, impl->vblank_event);
}

void GPU::RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window) {
    // Reset the renderer (this will destroy OpenGL resources)
    impl->renderer.reset();

    // Create a new renderer
    impl->renderer =
        VideoCore::CreateRenderer(emu_window, secondary_window, impl->pica, impl->system);
    impl->rasterizer = impl->renderer->Rasterizer();

    // Rebind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);

    // Update the sw_blitter with the new rasterizer
    impl->sw_blitter = std::make_unique<SwRenderer::SwBlitter>(impl->memory, impl->rasterizer);

    // Re-apply per-game configuration and reload disk shader cache
    u64 program_id{};
    impl->system.GetAppLoader().ReadProgramId(program_id);
    ApplyPerProgramSettings(program_id);
    if (Settings::values.use_disk_shader_cache) {
        impl->renderer->Rasterizer()->LoadDefaultDiskResources(false, nullptr);
    }

    // Mark ALL GPU registers as dirty so current state gets uploaded to new renderer
    impl->pica.dirty_regs.SetAllDirty();
    ++impl->pica.draw_merge_breaks;

    // Also mark shader setups as dirty so uniforms get re-uploaded and
    // stale pointers to the old rasterizer's JIT cache are cleared.
    impl->pica.vs_setup.uniforms_dirty = true;
    impl->pica.vs_setup.cached_shader = nullptr;
    impl->pica.gs_setup.uniforms_dirty = true;
    impl->pica.gs_setup.cached_shader = nullptr;

    // Mark all cached LUT/table state in pica as dirty
    impl->pica.lighting.lut_dirty = impl->pica.lighting.LutAllDirty;
    impl->pica.fog.lut_dirty = true;
    impl->pica.proctex.table_dirty = impl->pica.proctex.TableAllDirty;
}

void GPU::ReleaseRenderer() {
    // Just reset the renderer to release OpenGL resources
    // Don't null out rasterizer pointer as it will become dangling
    impl->renderer.reset();
    impl->sw_blitter.reset();
    LOG_INFO(HW_GPU, "Renderer released for context destroy");
}

template <class Archive>
void GPU::serialize(Archive& ar, const u32 file_version) {
    ar & impl->pica;
}

SERIALIZE_IMPL(GPU)

} // namespace VideoCore
