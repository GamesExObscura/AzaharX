// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <csignal>
#include <cstring>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/optimization_flags.h>
#include "common/assert.h"
#include "common/hacks/hack_list.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/dynarmic/arm_exclusive_monitor.h"
#include "core/arm/dynarmic/arm_tick_counts.h"
#include "core/core.h"
#include "core/core_timing.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

// Add missing typedefs for fixed-width types
#include <cstdint>
using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
#ifndef SIGILL
constexpr u32 SIGILL = 4;
#endif

#ifndef SIGTRAP
constexpr u32 SIGTRAP = 5;
#endif

namespace Core {

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() = default;

    std::optional<std::uint32_t> MemoryReadCode(VAddr vaddr) override {
        return memory.Read32OrNullopt(vaddr);
    }

    std::uint8_t MemoryRead8(VAddr vaddr) override {
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
        return memory.Read32(vaddr);
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override {
        return memory.Read64(vaddr);
    }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
        memory.Write32(vaddr, value);
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
        memory.Write64(vaddr, value);
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override {
        return memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override {
        return memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override {
        return memory.WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override {
        return memory.WriteExclusive64(vaddr, value, expected);
    }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Title-gated (GRACEFUL_JIT_FALLBACK, currently Trap Team only):
        // every other title keeps stock behavior — crash loudly — so an
        // unexpected unJITtable instruction is never silently skipped,
        // which would corrupt execution in ways that are far harder to
        // debug than this message.
        if (!Common::Hacks::g_graceful_jit_fallback.load(std::memory_order_relaxed)) {
            UNREACHABLE_MSG(
                "InterpeterFallback reached with pc = 0x{:08x}, code = 0x{:08x}, num = {}", pc,
                MemoryReadCode(pc).value(), num_instructions);
        }

        // Read the instruction that caused the fallback
        u32 instruction = MemoryReadCode(pc).value();

        // Log the fallback for debugging but don't crash
        LOG_WARNING(Debug, "Interpreter fallback: pc=0x{:08x}, instruction=0x{:08x}, count={}", pc,
                    instruction, num_instructions);

        // Handle basic instruction interpretation
        for (std::size_t i = 0; i < num_instructions; ++i) {
            u32 current_pc = pc + static_cast<u32>(i * 4);
            auto inst_opt = MemoryReadCode(current_pc);
            if (!inst_opt.has_value()) {
                break; // Skip if memory read fails
            }
            u32 inst = inst_opt.value();

            if (!ExecuteInstructionFallback(current_pc, inst)) {
                // If we can't handle the instruction, try to skip it gracefully
                LOG_ERROR(Debug, "Failed to interpret instruction 0x{:08x} at pc=0x{:08x}", inst,
                          current_pc);

                // Advance PC to next instruction and continue
                parent.SetPC(current_pc + 4);
                break;
            }
        }
    }

private:
    bool ExecuteInstructionFallback(u32 pc, u32 instruction) {
        // Decode and execute basic ARM instructions

        // Check for NOP instruction (0xe1a00000 or similar)
        if ((instruction & 0x0fff0fff) == 0x01a00000) {
            // NOP - do nothing, just advance PC
            parent.SetPC(pc + 4);
            return true;
        }

        // Handle ADD immediate: ADD Rd, Rn, #imm (0xf10c0040 matches this pattern)
        if ((instruction & 0x0fe00000) == 0x02800000) {
            u32 rn = (instruction >> 16) & 0xf;
            u32 rd = (instruction >> 12) & 0xf;
            u32 imm = instruction & 0xfff;

            u32 rn_val = (rn == 15) ? pc + 8 : parent.GetReg(rn);
            u32 result = rn_val + imm;

            if (rd != 15) {
                parent.SetReg(rd, result);
                parent.SetPC(pc + 4);
            } else {
                parent.SetPC(result);
            }
            return true;
        }

        // Handle SUB immediate: SUB Rd, Rn, #imm
        if ((instruction & 0x0fe00000) == 0x02400000) {
            u32 rn = (instruction >> 16) & 0xf;
            u32 rd = (instruction >> 12) & 0xf;
            u32 imm = instruction & 0xfff;

            u32 rn_val = (rn == 15) ? pc + 8 : parent.GetReg(rn);
            u32 result = rn_val - imm;

            if (rd != 15) {
                parent.SetReg(rd, result);
                parent.SetPC(pc + 4);
            } else {
                parent.SetPC(result);
            }
            return true;
        }

        // Handle MOV immediate: MOV Rd, #imm
        if ((instruction & 0x0fef0000) == 0x03a00000) {
            u32 rd = (instruction >> 12) & 0xf;
            u32 imm = instruction & 0xfff;

            if (rd != 15) {
                parent.SetReg(rd, imm);
                parent.SetPC(pc + 4);
            } else {
                parent.SetPC(imm);
            }
            return true;
        }

        // Handle LDR immediate: LDR Rd, [Rn, #imm]
        if ((instruction & 0x0c500000) == 0x04100000) {
            u32 rn = (instruction >> 16) & 0xf;
            u32 rd = (instruction >> 12) & 0xf;
            u32 imm = instruction & 0xfff;
            bool pre_index = (instruction & (1 << 24)) != 0;
            bool add = (instruction & (1 << 23)) != 0;

            u32 address = (rn == 15) ? pc + 8 : parent.GetReg(rn);
            if (pre_index) {
                address = add ? address + imm : address - imm;
            }

            u32 value = MemoryRead32(address);

            if (rd != 15) {
                parent.SetReg(rd, value);
                parent.SetPC(pc + 4);
            } else {
                parent.SetPC(value);
            }
            return true;
        }

        // Handle STR immediate: STR Rd, [Rn, #imm]
        if ((instruction & 0x0c500000) == 0x04000000) {
            u32 rn = (instruction >> 16) & 0xf;
            u32 rd = (instruction >> 12) & 0xf;
            u32 imm = instruction & 0xfff;
            bool pre_index = (instruction & (1 << 24)) != 0;
            bool add = (instruction & (1 << 23)) != 0;

            u32 address = (rn == 15) ? pc + 8 : parent.GetReg(rn);
            if (pre_index) {
                address = add ? address + imm : address - imm;
            }

            u32 value = (rd == 15) ? pc + 8 : parent.GetReg(rd);
            MemoryWrite32(address, value);

            parent.SetPC(pc + 4);
            return true;
        }

        // Handle B (branch): B label
        if ((instruction & 0x0f000000) == 0x0a000000) {
            s32 offset = (instruction & 0x00ffffff) << 2;
            if (offset & 0x02000000) {
                offset |= 0xfc000000; // Sign extend
            }

            u32 target = pc + 8 + offset;
            parent.SetPC(target);
            return true;
        }

        // Handle BL (branch with link): BL label
        if ((instruction & 0x0f000000) == 0x0b000000) {
            s32 offset = (instruction & 0x00ffffff) << 2;
            if (offset & 0x02000000) {
                offset |= 0xfc000000; // Sign extend
            }

            parent.SetReg(14, pc + 4); // Store return address in LR
            u32 target = pc + 8 + offset;
            parent.SetPC(target);
            return true;
        }

        // For unhandled instructions, just advance PC and continue
        LOG_DEBUG(Debug, "Unhandled instruction in fallback: 0x{:08x} at pc=0x{:08x}", instruction,
                  pc);
        parent.SetPC(pc + 4);
        return true;
    }

public:
    void CallSVC(std::uint32_t swi) override {
        svc_context.CallSVC(swi);
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
        case Dynarmic::A32::Exception::DecodeError:
        case Dynarmic::A32::Exception::NoExecuteFault:
            break;
        case Dynarmic::A32::Exception::Breakpoint:
#ifdef ENABLE_GDBSTUB
            if (GDBStub::IsConnected()) {
                parent.SetPC(pc);
                parent.ServeBreak(SIGTRAP);
                return;
            }
#endif
            break;
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadInstruction:
            return;
        }

        static constexpr auto ExceptionToString = [](Dynarmic::A32::Exception e) -> std::string {
            switch (e) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
                return "UndefinedInstruction";
            case Dynarmic::A32::Exception::UnpredictableInstruction:
                return "UnpredictableInstruction";
            case Dynarmic::A32::Exception::DecodeError:
                return "DecodeError";
            case Dynarmic::A32::Exception::NoExecuteFault:
                return "NoExecuteFault";
            case Dynarmic::A32::Exception::Breakpoint:
                return "Breakpoint";
            default:
                return fmt::format("Unknown({})", e);
            }
        };

        parent.SetPC(pc);
#ifdef ENABLE_GDBSTUB
        if (GDBStub::IsConnected()) {
            parent.ServeBreak(SIGILL);
        } else
#endif
        {
            std::string error;
            for (int i = 0; i < 16; i++) {
                error += fmt::format("r{:02d} = {:08X}\n", i, parent.GetReg(i));
            }
            error += fmt::format("ExceptionRaised(exception = {}, pc = {:08X})",
                                 ExceptionToString(exception), pc);
            parent.system.SetStatus(Core::System::ResultStatus::ErrorCoreExceptionRaised,
                                    error.c_str());
        }
    }

    void AddTicks(std::uint64_t ticks) override {
        parent.GetTimer().AddTicks(ticks);
    }
    std::uint64_t GetTicksRemaining() override {
        s64 ticks = parent.GetTimer().GetDowncount();
        return static_cast<u64>(ticks <= 0 ? 0 : ticks);
    }
    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
};

ARM_Dynarmic::ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                           std::shared_ptr<Core::Timing::Timer> timer_,
                           Core::ExclusiveMonitor& exclusive_monitor_)
    : ARM_Interface(core_id_, timer_), system(system_), memory(memory_),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)),
      exclusive_monitor{dynamic_cast<Core::DynarmicExclusiveMonitor&>(exclusive_monitor_)} {
    SetPageTable(memory.GetCurrentPageTable());
}

ARM_Dynarmic::~ARM_Dynarmic() = default;

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);
    if (break_flag) [[unlikely]] {
        return;
    }

    jit->Run();
}

void ARM_Dynarmic::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }

    jit->Step();
}

void ARM_Dynarmic::SetPC(u32 pc) {
    jit->Regs()[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return jit->Regs()[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return jit->Regs()[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    jit->Regs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return jit->ExtRegs()[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    jit->ExtRegs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return jit->Fpscr();
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }

    return UINT_MAX;
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        jit->SetFpscr(value);
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_Dynarmic::GetCPSR() const {
    return jit->Cpsr();
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    jit->SetCpsr(cpsr);
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return cp15_state.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return cp15_state.cp15_thread_uro;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }

    return 0;
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        cp15_state.cp15_thread_uprw = value;
        return;
    case CP15_THREAD_URO:
        cp15_state.cp15_thread_uro = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }
}

void ARM_Dynarmic::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = jit->Regs();
    ctx.cpsr = jit->Cpsr();
    ctx.fpu_registers = jit->ExtRegs();
    ctx.fpscr = jit->Fpscr();
    ctx.fpexc = fpexc;
}

void ARM_Dynarmic::LoadContext(const ThreadContext& ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->SetCpsr(ctx.cpsr);
    jit->ExtRegs() = ctx.fpu_registers;
    jit->SetFpscr(ctx.fpscr);
    fpexc = ctx.fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
    if (jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
    for (const auto& j : jits) {
        j.second->ClearCache();
    }
}

void ARM_Dynarmic::InvalidateCacheRange(u32 start_address, std::size_t length) {
    jit->InvalidateCacheRange(start_address, length);
}

void ARM_Dynarmic::ClearExclusiveState() {
    jit->ClearExclusiveState();
}

std::shared_ptr<Memory::PageTable> ARM_Dynarmic::GetPageTable() const {
    return current_page_table;
}

void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    ThreadContext ctx{};
    if (jit) {
        SaveContext(ctx);
    }

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        LoadContext(ctx);
        return;
    }

    auto new_jit = MakeJit();
    jit = new_jit.get();
    LoadContext(ctx);
    jits.emplace(current_page_table, std::move(new_jit));
}

void ARM_Dynarmic::ServeBreak([[maybe_unused]] int signal) {
#ifdef ENABLE_GDBSTUB
    GDBStub::Break(signal);
#endif
}

std::unique_ptr<Dynarmic::A32::Jit> ARM_Dynarmic::MakeJit() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = cb.get();
    // ACCURATE_JIT_MEMORY hack: leave config.page_table null so every
    // load/store is routed through the memory callbacks, which respect
    // RasterizerCachedMemory page attributes. Fixes titles whose CPU
    // writes to rasterizer-cached pages (texture data, PICA command
    // buffer patches) were otherwise lost through the inline fast
    // path. The flag is set during title load, before the app
    // process's page table triggers this MakeJit.
    const bool accurate_memory =
        Common::Hacks::g_accurate_jit_memory.load(std::memory_order_relaxed);
    if (current_page_table && !accurate_memory) {
        config.page_table = &current_page_table->GetPointerArray();
    }
    if (accurate_memory) {
        LOG_INFO(Core_ARM11, "ACCURATE_JIT_MEMORY active: JIT built without page table");
    }
    config.coprocessors[15] = std::make_shared<DynarmicCP15>(cp15_state);
    config.define_unpredictable_behaviour = true;

    // Multi-process state
    config.processor_id = GetID();
    config.global_monitor = &exclusive_monitor.monitor;

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

} // namespace Core