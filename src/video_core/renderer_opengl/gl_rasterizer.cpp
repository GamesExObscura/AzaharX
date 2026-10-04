// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <numeric>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/literals.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/microprofile.h"
#include "core/loader/loader.h"
#include "common/hacks/hack_manager.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/pica_to_gl.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/shader/generator/shader_gen.h"
#include "video_core/texture/texture_decode.h"

namespace OpenGL {

namespace {

MICROPROFILE_DEFINE(OpenGL_VAO, "OpenGL", "Vertex Array Setup", MP_RGB(255, 128, 0));
MICROPROFILE_DEFINE(OpenGL_VS, "OpenGL", "Vertex Shader Setup", MP_RGB(192, 128, 128));
MICROPROFILE_DEFINE(OpenGL_GS, "OpenGL", "Geometry Shader Setup", MP_RGB(128, 192, 128));
MICROPROFILE_DEFINE(OpenGL_Drawing, "OpenGL", "Drawing", MP_RGB(128, 128, 192));
MICROPROFILE_DEFINE(OpenGL_Display, "OpenGL", "Display", MP_RGB(128, 128, 192));

using VideoCore::SurfaceType;
using namespace Common::Literals;
using namespace Pica::Shader::Generator;

constexpr std::size_t VERTEX_BUFFER_SIZE = 16_MiB;
constexpr std::size_t INDEX_BUFFER_SIZE = 2_MiB;
constexpr std::size_t UNIFORM_BUFFER_SIZE = 8_MiB;
constexpr std::size_t TEXTURE_BUFFER_SIZE = 2_MiB;

GLenum MakePrimitiveMode(Pica::PipelineRegs::TriangleTopology topology) {
    switch (topology) {
    case Pica::PipelineRegs::TriangleTopology::Shader:
    case Pica::PipelineRegs::TriangleTopology::List:
        return GL_TRIANGLES;
    case Pica::PipelineRegs::TriangleTopology::Fan:
        return GL_TRIANGLE_FAN;
    case Pica::PipelineRegs::TriangleTopology::Strip:
        return GL_TRIANGLE_STRIP;
    default:
        UNREACHABLE();
    }
    return GL_TRIANGLES;
}

GLenum MakeAttributeType(Pica::PipelineRegs::VertexAttributeFormat format) {
    switch (format) {
    case Pica::PipelineRegs::VertexAttributeFormat::BYTE:
        return GL_BYTE;
    case Pica::PipelineRegs::VertexAttributeFormat::UBYTE:
        return GL_UNSIGNED_BYTE;
    case Pica::PipelineRegs::VertexAttributeFormat::SHORT:
        return GL_SHORT;
    case Pica::PipelineRegs::VertexAttributeFormat::FLOAT:
        return GL_FLOAT;
    }
    return GL_UNSIGNED_BYTE;
}

[[nodiscard]] GLsizeiptr TextureBufferSize(const Driver& driver, bool is_lf) {
    // Use the smallest texel size from the texel views
    // which corresponds to GL_RG32F
    GLint max_texel_buffer_size;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &max_texel_buffer_size);
    GLsizeiptr candidate = std::min<GLsizeiptr>(max_texel_buffer_size * 8ULL, TEXTURE_BUFFER_SIZE);

    if (driver.HasBug(DriverBug::SlowTextureBufferWithBigSize) && !is_lf) {
        constexpr GLsizeiptr FIXUP_TEXTURE_BUFFER_SIZE = static_cast<GLsizeiptr>(1 << 14); // 16384
        return FIXUP_TEXTURE_BUFFER_SIZE;
    }

    return candidate;
}

} // Anonymous namespace

RasterizerOpenGL::RasterizerOpenGL(Memory::MemorySystem& memory, Pica::PicaCore& pica,
                                   VideoCore::CustomTexManager& custom_tex_manager,
                                   VideoCore::RendererBase& renderer, Driver& driver_)
    : VideoCore::RasterizerAccelerated{memory, pica}, driver{driver_},
      render_window{renderer.GetRenderWindow()}, runtime{driver, renderer},
      res_cache{memory, custom_tex_manager, runtime, regs, renderer},
      vertex_buffer{driver, GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE},
      uniform_buffer{driver, GL_UNIFORM_BUFFER, UNIFORM_BUFFER_SIZE},
      index_buffer{driver, GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE},
      texture_buffer{driver, GL_TEXTURE_BUFFER, TextureBufferSize(driver, false)},
      texture_lf_buffer{driver, GL_TEXTURE_BUFFER, TextureBufferSize(driver, true)} {

    // Clipping plane 0 is always enabled for PICA fixed clip plane z <= 0
    state.clip_distance[0] = true;

    // Generate VAO
    sw_vao.Create();
    hw_vao.Create();

    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniform_buffer_alignment);
    uniform_size_aligned_vs_pica =
        Common::AlignUp<std::size_t>(sizeof(VSPicaUniformData), uniform_buffer_alignment);
    uniform_size_aligned_vs =
        Common::AlignUp<std::size_t>(sizeof(VSUniformData), uniform_buffer_alignment);
    uniform_size_aligned_fs =
        Common::AlignUp<std::size_t>(sizeof(FSUniformData), uniform_buffer_alignment);

    // Set vertex attributes for software shader path
    state.draw.vertex_array = sw_vao.handle;
    state.draw.vertex_buffer = vertex_buffer.GetHandle();
    state.Apply();

    glVertexAttribPointer(ATTRIBUTE_POSITION, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, position));
    glEnableVertexAttribArray(ATTRIBUTE_POSITION);

    glVertexAttribPointer(ATTRIBUTE_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, color));
    glEnableVertexAttribArray(ATTRIBUTE_COLOR);

    glVertexAttribPointer(ATTRIBUTE_TEXCOORD0, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord0));
    glVertexAttribPointer(ATTRIBUTE_TEXCOORD1, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord1));
    glVertexAttribPointer(ATTRIBUTE_TEXCOORD2, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord2));
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD0);
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD1);
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD2);

    glVertexAttribPointer(ATTRIBUTE_TEXCOORD0_W, 1, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord0_w));
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD0_W);

    glVertexAttribPointer(ATTRIBUTE_NORMQUAT, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, normquat));
    glEnableVertexAttribArray(ATTRIBUTE_NORMQUAT);

    glVertexAttribPointer(ATTRIBUTE_VIEW, 3, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, view));
    glEnableVertexAttribArray(ATTRIBUTE_VIEW);

    // Allocate and bind texture buffer lut textures
    texture_buffer_lut_lf.Create();
    texture_buffer_lut_rg.Create();
    texture_buffer_lut_rgba.Create();
    state.texture_buffer_lut_lf.texture_buffer = texture_buffer_lut_lf.handle;
    state.texture_buffer_lut_rg.texture_buffer = texture_buffer_lut_rg.handle;
    state.texture_buffer_lut_rgba.texture_buffer = texture_buffer_lut_rgba.handle;
    state.Apply();
    glActiveTexture(TextureUnits::TextureBufferLUT_LF.Enum());
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32F, texture_lf_buffer.GetHandle());
    glActiveTexture(TextureUnits::TextureBufferLUT_RG.Enum());
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32F, texture_buffer.GetHandle());
    glActiveTexture(TextureUnits::TextureBufferLUT_RGBA.Enum());
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, texture_buffer.GetHandle());

    // Bind index buffer for hardware shader path
    state.draw.vertex_array = hw_vao.handle;
    state.Apply();
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer.GetHandle());

    glEnable(GL_BLEND);
}

RasterizerOpenGL::~RasterizerOpenGL() = default;

void RasterizerOpenGL::TickFrame() {
    FlushDrawMerge();
    res_cache.TickFrame();
}

void RasterizerOpenGL::LoadDefaultDiskResources(
    const std::atomic_bool& stop_loading, const VideoCore::DiskResourceLoadCallback& callback) {
    FlushDrawMerge();
    // First element in vector is the default one and cannot be removed.
    u64 program_id;
    if (Core::System::GetInstance().GetAppLoader().ReadProgramId(program_id) !=
        Loader::ResultStatus::Success) {
        program_id = 0;
    }

    shader_managers.clear();
    curr_shader_manager = shader_managers.emplace_back(std::make_shared<ShaderProgramManager>(
        render_window, driver, program_id, !driver.IsOpenGLES()));

    curr_shader_manager->LoadDiskCache(stop_loading, callback, accurate_mul);
}

void RasterizerOpenGL::SwitchDiskResources(u64 title_id) {
    FlushDrawMerge();
    // NOTE: curr_shader_manager can be null if emulation restarted without calling
    // LoadDefaultDiskResources

    // Check if the current manager is for the specified TID.
    if (curr_shader_manager && curr_shader_manager->GetProgramID() == title_id) {
        return;
    }

    // Search for an existing manager
    size_t new_pos = 0;
    for (new_pos = 0; new_pos < shader_managers.size(); new_pos++) {
        if (shader_managers[new_pos]->GetProgramID() == title_id) {
            break;
        }
    }
    // Manager does not exist, create it and append to the end
    if (new_pos >= shader_managers.size()) {
        new_pos = shader_managers.size();
        auto& new_manager = shader_managers.emplace_back(std::make_shared<ShaderProgramManager>(
            render_window, driver, title_id, !driver.IsOpenGLES()));

        if (switch_disk_resources_callback) {
            switch_disk_resources_callback(VideoCore::LoadCallbackStage::Prepare, 0, 0, "");
        }

        std::atomic_bool stop_loading;
        new_manager->LoadDiskCache(stop_loading, switch_disk_resources_callback, accurate_mul);

        if (switch_disk_resources_callback) {
            switch_disk_resources_callback(VideoCore::LoadCallbackStage::Complete, 0, 0, "");
        }
    }

    auto is_applet = [](u64 tid) {
        constexpr u32 APPLET_TID_HIGH = 0x00040030;
        return static_cast<u32>(tid >> 32) == APPLET_TID_HIGH;
    };

    bool prev_applet = curr_shader_manager ? is_applet(curr_shader_manager->GetProgramID()) : false;
    bool new_applet = is_applet(shader_managers[new_pos]->GetProgramID());
    curr_shader_manager = shader_managers[new_pos];

    if (prev_applet) {
        // If we came from an applet, clean up all other applets
        for (auto it = shader_managers.begin(); it != shader_managers.end();) {
            if (it == shader_managers.begin() || *it == curr_shader_manager ||
                !is_applet((*it)->GetProgramID())) {
                it++;
                continue;
            }
            it = shader_managers.erase(it);
        }
    }
    if (!new_applet) {
        // If we are going into a non-applet, clean up everything
        for (auto it = shader_managers.begin(); it != shader_managers.end();) {
            if (it == shader_managers.begin() || *it == curr_shader_manager) {
                it++;
                continue;
            }
            it = shader_managers.erase(it);
        }
    }
}

void RasterizerOpenGL::SyncDrawState() {
    SyncDrawUniforms();

    // SyncClipEnabled();
    state.clip_distance[1] = regs.rasterizer.clip_enable != 0;
    // SyncCullMode();
    // Cull mode 3 is not a valid PICA value. Citra 2104 and pre-#1059 Azahar ignored it and kept
    // the previous cull state; since #1059 it enables culling with the opposite front face, which
    // culls every triangle for titles that write it (title-gated — see
    // HackType::CULL_MODE_INVALID_KEEPS_STATE).
    const bool keep_cull_state =
        static_cast<u32>(regs.rasterizer.cull_mode.Value()) > 2 &&
        Common::Hacks::g_cull_mode_invalid_keeps_state.load(std::memory_order_relaxed);
    if (!keep_cull_state) {
        state.cull.enabled = regs.rasterizer.cull_mode != Pica::RasterizerRegs::CullMode::KeepAll;
        if (state.cull.enabled) {
            state.cull.front_face =
                regs.rasterizer.cull_mode == Pica::RasterizerRegs::CullMode::KeepClockWise
                    ? GL_CW
                    : GL_CCW;
        }
    }
    // If the framebuffer is flipped, vertex shader flips vertex y, so invert culling
    const bool is_flipped = regs.framebuffer.framebuffer.IsFlipped();
    state.cull.mode = is_flipped && state.cull.enabled ? GL_FRONT : GL_BACK;
    // SyncBlendEnabled();
    state.blend.enabled = (regs.framebuffer.output_merger.alphablend_enable == 1);
    // SyncBlendFuncs();
    const bool has_minmax_factor = driver.HasBlendMinMaxFactor();
    state.blend.rgb_equation = PicaToGL::BlendEquation(
        regs.framebuffer.output_merger.alpha_blending.blend_equation_rgb, has_minmax_factor);
    state.blend.a_equation = PicaToGL::BlendEquation(
        regs.framebuffer.output_merger.alpha_blending.blend_equation_a, has_minmax_factor);
    state.blend.src_rgb_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_source_rgb);
    state.blend.dst_rgb_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_dest_rgb);
    state.blend.src_a_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_source_a);
    state.blend.dst_a_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_dest_a);
    if (!has_minmax_factor) {
        // Blending with min/max equations is emulated in the fragment shader so
        // configure blending to not modify the incoming fragment color.
        emulate_minmax_blend = false;
        if (state.EmulateColorBlend()) {
            emulate_minmax_blend = true;
            state.blend.rgb_equation = GL_FUNC_ADD;
            state.blend.src_rgb_func = GL_ONE;
            state.blend.dst_rgb_func = GL_ZERO;
        }
        if (state.EmulateAlphaBlend()) {
            emulate_minmax_blend = true;
            state.blend.a_equation = GL_FUNC_ADD;
            state.blend.src_a_func = GL_ONE;
            state.blend.dst_a_func = GL_ZERO;
        }
    }
    // SyncBlendColor();
    const auto blend_color = PicaToGL::ColorRGBA8(regs.framebuffer.output_merger.blend_const.raw);
    state.blend.color.red = blend_color[0];
    state.blend.color.green = blend_color[1];
    state.blend.color.blue = blend_color[2];
    state.blend.color.alpha = blend_color[3];
    // SyncLogicOp();
    // SyncColorWriteMask();
    state.logic_op = PicaToGL::LogicOp(regs.framebuffer.output_merger.logic_op);
    if (driver.IsOpenGLES() && !regs.framebuffer.output_merger.alphablend_enable &&
        regs.framebuffer.output_merger.logic_op == Pica::FramebufferRegs::LogicOp::NoOp) {
        // Color output is disabled by logic operation. We use color write mask to skip
        // color but allow depth write.
        state.color_mask = {};
    } else {
        auto is_color_write_enabled = [&](u32 value) {
            return (regs.framebuffer.framebuffer.allow_color_write != 0 && value != 0) ? GL_TRUE
                                                                                       : GL_FALSE;
        };
        state.color_mask.red_enabled =
            is_color_write_enabled(regs.framebuffer.output_merger.red_enable);
        state.color_mask.green_enabled =
            is_color_write_enabled(regs.framebuffer.output_merger.green_enable);
        state.color_mask.blue_enabled =
            is_color_write_enabled(regs.framebuffer.output_merger.blue_enable);
        state.color_mask.alpha_enabled =
            is_color_write_enabled(regs.framebuffer.output_merger.alpha_enable);
    }
    // SyncStencilTest();
    state.stencil.test_enabled =
        regs.framebuffer.output_merger.stencil_test.enable &&
        regs.framebuffer.framebuffer.depth_format == Pica::FramebufferRegs::DepthFormat::D24S8;
    state.stencil.test_func =
        PicaToGL::CompareFunc(regs.framebuffer.output_merger.stencil_test.func);
    state.stencil.test_ref = regs.framebuffer.output_merger.stencil_test.reference_value;
    state.stencil.test_mask = regs.framebuffer.output_merger.stencil_test.input_mask;
    state.stencil.action_stencil_fail =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_stencil_fail);
    state.stencil.action_depth_fail =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_depth_fail);
    state.stencil.action_depth_pass =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_depth_pass);
    // SyncDepthTest();
    state.depth.test_enabled = regs.framebuffer.output_merger.depth_test_enable == 1 ||
                               regs.framebuffer.output_merger.depth_write_enable == 1;
    state.depth.test_func =
        regs.framebuffer.output_merger.depth_test_enable == 1
            ? PicaToGL::CompareFunc(regs.framebuffer.output_merger.depth_test_func)
            : GL_ALWAYS;
    // SyncStencilWriteMask();
    state.stencil.write_mask =
        (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0)
            ? static_cast<GLuint>(regs.framebuffer.output_merger.stencil_test.write_mask)
            : 0;
    // SyncDepthWriteMask();
    state.depth.write_mask = (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0 &&
                              regs.framebuffer.output_merger.depth_write_enable)
                                 ? GL_TRUE
                                 : GL_FALSE;
}

void RasterizerOpenGL::SetupVertexArray(u8* array_ptr, GLintptr buffer_offset,
                                        GLuint vs_input_index_min, GLuint vs_input_index_max) {
    MICROPROFILE_SCOPE(OpenGL_VAO);
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;
    PAddr base_address = vertex_attributes.GetPhysicalBaseAddress();

    state.draw.vertex_array = hw_vao.handle;
    state.draw.vertex_buffer = vertex_buffer.GetHandle();
    state.Apply();

    std::array<bool, 16> enable_attributes{};

    for (const auto& loader : vertex_attributes.attribute_loaders) {
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }

        u32 offset = 0;
        for (u32 comp = 0; comp < loader.component_count && comp < 12; ++comp) {
            u32 attribute_index = loader.GetComponent(comp);
            if (attribute_index < 12) {
                if (vertex_attributes.GetNumElements(attribute_index) != 0) {
                    offset = Common::AlignUp(
                        offset, vertex_attributes.GetElementSizeInBytes(attribute_index));

                    u32 input_reg = regs.vs.GetRegisterForAttribute(attribute_index);
                    GLint size = vertex_attributes.GetNumElements(attribute_index);
                    GLenum type = MakeAttributeType(vertex_attributes.GetFormat(attribute_index));
                    GLsizei stride = loader.byte_count;
                    glVertexAttribPointer(input_reg, size, type, GL_FALSE, stride,
                                          reinterpret_cast<GLvoid*>(buffer_offset + offset));
                    enable_attributes[input_reg] = true;

                    offset += vertex_attributes.GetStride(attribute_index);
                }
            } else {
                // Attribute ids 12, 13, 14 and 15 signify 4, 8, 12 and 16-byte paddings,
                // respectively
                offset = Common::AlignUp(offset, 4);
                offset += (attribute_index - 11) * 4;
            }
        }

        const PAddr data_addr =
            base_address + loader.data_offset + (vs_input_index_min * loader.byte_count);

        const u32 vertex_num = vs_input_index_max - vs_input_index_min + 1;
        const u32 data_size = loader.byte_count * vertex_num;

        res_cache.FlushRegion(data_addr, data_size);
        std::memcpy(array_ptr, memory.GetPhysicalPointer(data_addr), data_size);

        array_ptr += data_size;
        buffer_offset += data_size;
    }

    for (std::size_t i = 0; i < enable_attributes.size(); ++i) {
        if (enable_attributes[i] != hw_vao_enabled_attributes[i]) {
            if (enable_attributes[i]) {
                glEnableVertexAttribArray(static_cast<GLuint>(i));
            } else {
                glDisableVertexAttribArray(static_cast<GLuint>(i));
            }
            hw_vao_enabled_attributes[i] = enable_attributes[i];
        }

        if (vertex_attributes.IsDefaultAttribute(i)) {
            const u32 reg = regs.vs.GetRegisterForAttribute(i);
            if (!enable_attributes[reg]) {
                const auto& attr = pica.input_default_attributes[i];
                glVertexAttrib4f(reg, attr.x.ToFloat32(), attr.y.ToFloat32(), attr.z.ToFloat32(),
                                 attr.w.ToFloat32());
            }
        }
    }
}

s32 RasterizerOpenGL::SingleAttributeLoader() const {
    s32 found = -1;
    const auto& loaders = regs.pipeline.vertex_attributes.attribute_loaders;
    for (u32 i = 0; i < std::size(loaders); ++i) {
        if (loaders[i].component_count == 0 || loaders[i].byte_count == 0) {
            continue;
        }
        if (found != -1) {
            return -1;
        }
        found = static_cast<s32>(i);
    }
    return found;
}

void RasterizerOpenGL::SetupVertexArrayZeroBase(u32 loader_index, u8* array_ptr,
                                                GLuint vs_input_index_min,
                                                GLuint vs_input_index_max) {
    MICROPROFILE_SCOPE(OpenGL_VAO);
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;
    const auto& loader = vertex_attributes.attribute_loaders[loader_index];

    state.draw.vertex_array = hw_vao.handle;
    state.draw.vertex_buffer = vertex_buffer.GetHandle();
    state.Apply();

    // The same attribute walk as SetupVertexArray, collected instead of applied.
    std::array<bool, 16> enable_attributes{};
    std::array<ZeroBaseAttrib, 12> attribs{};
    u32 count = 0;
    u32 offset = 0;
    for (u32 comp = 0; comp < loader.component_count && comp < 12; ++comp) {
        const u32 attribute_index = loader.GetComponent(comp);
        if (attribute_index < 12) {
            if (vertex_attributes.GetNumElements(attribute_index) != 0) {
                offset = Common::AlignUp(offset,
                                         vertex_attributes.GetElementSizeInBytes(attribute_index));
                const u32 input_reg = regs.vs.GetRegisterForAttribute(attribute_index);
                attribs[count++] = {
                    .input_reg = input_reg,
                    .size = static_cast<GLint>(vertex_attributes.GetNumElements(attribute_index)),
                    .type = MakeAttributeType(vertex_attributes.GetFormat(attribute_index)),
                    .stride = static_cast<GLsizei>(loader.byte_count),
                    .offset = offset,
                };
                enable_attributes[input_reg] = true;
                offset += vertex_attributes.GetStride(attribute_index);
            }
        } else {
            // Attribute ids 12, 13, 14 and 15 signify 4, 8, 12 and 16-byte paddings
            offset = Common::AlignUp(offset, 4);
            offset += (attribute_index - 11) * 4;
        }
    }

    if (!zero_base_layout.valid || zero_base_layout.count != count ||
        !std::equal(attribs.begin(), attribs.begin() + count, zero_base_layout.attribs.begin())) {
        for (u32 i = 0; i < count; ++i) {
            const ZeroBaseAttrib& attrib = attribs[i];
            glVertexAttribPointer(attrib.input_reg, attrib.size, attrib.type, GL_FALSE,
                                  attrib.stride, reinterpret_cast<GLvoid*>(attrib.offset));
        }
        zero_base_layout.valid = true;
        zero_base_layout.count = count;
        zero_base_layout.attribs = attribs;
    }

    const PAddr data_addr = vertex_attributes.GetPhysicalBaseAddress() + loader.data_offset +
                            (vs_input_index_min * loader.byte_count);
    const u32 vertex_num = vs_input_index_max - vs_input_index_min + 1;
    const u32 data_size = loader.byte_count * vertex_num;
    res_cache.FlushRegion(data_addr, data_size);
    std::memcpy(array_ptr, memory.GetPhysicalPointer(data_addr), data_size);

    for (std::size_t i = 0; i < enable_attributes.size(); ++i) {
        if (enable_attributes[i] != hw_vao_enabled_attributes[i]) {
            if (enable_attributes[i]) {
                glEnableVertexAttribArray(static_cast<GLuint>(i));
            } else {
                glDisableVertexAttribArray(static_cast<GLuint>(i));
            }
            hw_vao_enabled_attributes[i] = enable_attributes[i];
        }

        if (vertex_attributes.IsDefaultAttribute(i)) {
            const u32 reg = regs.vs.GetRegisterForAttribute(i);
            if (!enable_attributes[reg]) {
                const auto& attr = pica.input_default_attributes[i];
                const Common::Vec4f value{attr.x.ToFloat32(), attr.y.ToFloat32(),
                                          attr.z.ToFloat32(), attr.w.ToFloat32()};
                if (!zero_base_layout.default_known[reg] ||
                    !(zero_base_layout.default_value[reg] == value)) {
                    glVertexAttrib4f(reg, value.x, value.y, value.z, value.w);
                    zero_base_layout.default_known[reg] = true;
                    zero_base_layout.default_value[reg] = value;
                }
            }
        }
    }
}

bool RasterizerOpenGL::SetupVertexShader() {
    MICROPROFILE_SCOPE(OpenGL_VS);
    return curr_shader_manager->UseProgrammableVertexShader(regs, pica.vs_setup, accurate_mul);
}

bool RasterizerOpenGL::SetupGeometryShader() {
    MICROPROFILE_SCOPE(OpenGL_GS);

    if (regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No) {
        LOG_ERROR(Render_OpenGL, "Accelerate draw doesn't support geometry shader");
        return false;
    }

    // Enable the quaternion fix-up geometry-shader only if we are actually doing per-fragment
    // lighting and care about proper quaternions. Otherwise just use standard vertex+fragment
    // shaders
    if (regs.lighting.disable) {
        curr_shader_manager->UseTrivialGeometryShader();
    } else {
        curr_shader_manager->UseFixedGeometryShader(regs);
    }

    return true;
}

bool RasterizerOpenGL::AccelerateDrawBatch(bool is_indexed) {
    if (regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No) {
        if (regs.pipeline.gs_config.mode != Pica::PipelineRegs::GSMode::Point) {
            return false;
        }
        if (regs.pipeline.triangle_topology != Pica::PipelineRegs::TriangleTopology::Shader) {
            return false;
        }
    }

    // DRAW_LOOKUP_REUSE (title-gated): merge with the pending draws when only the vertices
    // differ; otherwise issue them before this draw touches any GL state.
    if (!draw_merge.counts.empty()) {
        if (TryAppendDrawMerge(is_indexed)) {
            return true;
        }
        FlushDrawMerge();
    }

    if (!SetupVertexShader()) {
        return false;
    }

    if (!SetupGeometryShader()) {
        return false;
    }

    return Draw(true, is_indexed);
}

bool RasterizerOpenGL::AccelerateDrawBatchInternal(bool is_indexed) {
    const GLenum primitive_mode = MakePrimitiveMode(regs.pipeline.triangle_topology);
    auto [vs_input_index_min, vs_input_index_max, vs_input_size] = AnalyzeVertexArray(is_indexed);

    if (vs_input_size > VERTEX_BUFFER_SIZE) {
        LOG_WARNING(Render_OpenGL, "Too large vertex input size {}", vs_input_size);
        return false;
    }

    state.draw.vertex_buffer = vertex_buffer.GetHandle();
    state.Apply();

    u8* buffer_ptr;
    GLintptr buffer_offset;
    // Vertex index the draw's first uploaded vertex sits at (non-zero only for the
    // DRAW_LOOKUP_REUSE zero-base layout below).
    GLint first_vertex = 0;
    const s32 single_loader = Common::Hacks::g_draw_lookup_reuse.load(std::memory_order_relaxed)
                                  ? SingleAttributeLoader()
                                  : -1;
    if (single_loader >= 0) {
        // DRAW_LOOKUP_REUSE (title-gated): one interleaved loader. Align the upload to the
        // vertex stride and keep the attribute pointers at offset 0; the draw then reaches its
        // vertices through first/base vertex, so consecutive draws with the same layout make
        // no vertex-array state change at all.
        const u32 stride =
            regs.pipeline.vertex_attributes.attribute_loaders[single_loader].byte_count;
        const GLintptr alignment = std::lcm<GLintptr>(stride, 4);
        std::tie(buffer_ptr, buffer_offset, std::ignore) =
            vertex_buffer.Map(vs_input_size, alignment);
        SetupVertexArrayZeroBase(static_cast<u32>(single_loader), buffer_ptr, vs_input_index_min,
                                 vs_input_index_max);
        vertex_buffer.Unmap(vs_input_size);
        first_vertex = static_cast<GLint>(buffer_offset / stride);
    } else {
        zero_base_layout.valid = false;
        zero_base_layout.default_known = {};
        std::tie(buffer_ptr, buffer_offset, std::ignore) = vertex_buffer.Map(vs_input_size, 4);
        SetupVertexArray(buffer_ptr, buffer_offset, vs_input_index_min, vs_input_index_max);
        vertex_buffer.Unmap(vs_input_size);
    }

    curr_shader_manager->ApplyTo(state, accurate_mul);
    state.Apply();

    if (is_indexed) {
        bool index_u16 = regs.pipeline.index_array.format != 0;
        std::size_t index_buffer_size = regs.pipeline.num_vertices * (index_u16 ? 2 : 1);

        if (index_buffer_size > INDEX_BUFFER_SIZE) {
            LOG_WARNING(Render_OpenGL, "Too large index input size {}", index_buffer_size);
            return false;
        }

        const u8* index_data =
            memory.GetPhysicalPointer(regs.pipeline.vertex_attributes.GetPhysicalBaseAddress() +
                                      regs.pipeline.index_array.offset);
        std::tie(buffer_ptr, buffer_offset, std::ignore) = index_buffer.Map(index_buffer_size, 4);
        std::memcpy(buffer_ptr, index_data, index_buffer_size);
        index_buffer.Unmap(index_buffer_size);

        glDrawRangeElementsBaseVertex(
            primitive_mode, vs_input_index_min, vs_input_index_max, regs.pipeline.num_vertices,
            index_u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
            reinterpret_cast<const void*>(buffer_offset),
            first_vertex - static_cast<GLint>(vs_input_index_min));
    } else if (single_loader >= 0 && !regs.framebuffer.IsShadowRendering()) {
        // DRAW_LOOKUP_REUSE (title-gated): hold the draw so the following draws that differ
        // only in their vertices can be issued with it as one glMultiDrawArrays.
        draw_merge.mode = primitive_mode;
        draw_merge.loader = single_loader;
        draw_merge.stride =
            regs.pipeline.vertex_attributes.attribute_loaders[single_loader].byte_count;
        draw_merge.pica_breaks = pica.draw_merge_breaks;
        draw_merge.cache_generation = res_cache.MutationGeneration();
        draw_merge.firsts.push_back(first_vertex);
        draw_merge.counts.push_back(static_cast<GLsizei>(regs.pipeline.num_vertices));
    } else {
        glDrawArrays(primitive_mode, first_vertex, regs.pipeline.num_vertices);
    }
    return true;
}

bool RasterizerOpenGL::TryAppendDrawMerge(bool is_indexed) {
    // Every input of the GL state must be unchanged since the first merged draw: render
    // registers (draw_merge_breaks), VS uniforms/code/swizzles, and the rasterizer cache.
    const auto& pipeline = regs.pipeline;
    if (is_indexed || pipeline.num_vertices == 0 ||
        pica.draw_merge_breaks != draw_merge.pica_breaks ||
        res_cache.MutationGeneration() != draw_merge.cache_generation ||
        pica.vs_setup.uniforms_dirty || pica.vs_setup.IsProgramCodeHashDirty() ||
        pica.vs_setup.IsSwizzleDataHashDirty() ||
        MakePrimitiveMode(pipeline.triangle_topology) != draw_merge.mode ||
        SingleAttributeLoader() != draw_merge.loader) {
        return false;
    }

    const auto [vs_input_index_min, vs_input_index_max, vs_input_size] =
        AnalyzeVertexArray(false);
    if (vs_input_size > VERTEX_BUFFER_SIZE) {
        return false;
    }
    const auto& loader = pipeline.vertex_attributes.attribute_loaders[draw_merge.loader];
    const PAddr data_addr = pipeline.vertex_attributes.GetPhysicalBaseAddress() +
                            loader.data_offset + (vs_input_index_min * loader.byte_count);
    const u32 data_size = loader.byte_count * (vs_input_index_max - vs_input_index_min + 1);
    // GPU-written vertex data would need a download, and a wrap would invalidate the vertices
    // the pending draws read: issue them first.
    const GLintptr alignment = std::lcm<GLintptr>(draw_merge.stride, 4);
    if (res_cache.IsRegionGpuDirty(data_addr, data_size) ||
        vertex_buffer.WouldWrap(vs_input_size, alignment)) {
        return false;
    }

    const auto [buffer_ptr, buffer_offset, invalidated] =
        vertex_buffer.Map(vs_input_size, alignment);
    std::memcpy(buffer_ptr, memory.GetPhysicalPointer(data_addr), data_size);
    vertex_buffer.Unmap(vs_input_size);

    draw_merge.firsts.push_back(static_cast<GLint>(buffer_offset / draw_merge.stride));
    draw_merge.counts.push_back(static_cast<GLsizei>(pipeline.num_vertices));
    return true;
}

void RasterizerOpenGL::FlushDrawMerge() {
    if (draw_merge.counts.empty()) {
        return;
    }
    // Nothing has touched the GL state since the first merged draw set it up.
    if (draw_merge.counts.size() == 1) {
        glDrawArrays(draw_merge.mode, draw_merge.firsts[0], draw_merge.counts[0]);
    } else {
        glMultiDrawArrays(draw_merge.mode, draw_merge.firsts.data(), draw_merge.counts.data(),
                          static_cast<GLsizei>(draw_merge.counts.size()));
    }
    draw_merge.firsts.clear();
    draw_merge.counts.clear();
}

void RasterizerOpenGL::DrawTriangles() {
    if (vertex_batch.empty())
        return;
    FlushDrawMerge();
    Draw(false, false);
}

RasterizerOpenGL::DrawLookupKey RasterizerOpenGL::MakeDrawLookupKey(bool using_color_fb,
                                                                    bool using_depth_fb) const {
    static_assert(sizeof(regs.framebuffer.framebuffer) == 16 * sizeof(u32));
    static_assert(sizeof(Pica::TexturingRegs::TextureConfig) == 5 * sizeof(u32));
    DrawLookupKey key;
    std::memcpy(key.framebuffer.data(), &regs.framebuffer.framebuffer, 16 * sizeof(u32));

    const auto& rasterizer = regs.rasterizer;
    std::memcpy(&key.viewport_scissor[0], &rasterizer.viewport_size_x, sizeof(u32));
    std::memcpy(&key.viewport_scissor[1], &rasterizer.viewport_size_y, sizeof(u32));
    std::memcpy(&key.viewport_scissor[2], &rasterizer.scissor_test, 3 * sizeof(u32));
    std::memcpy(&key.viewport_scissor[5], &rasterizer.viewport_corner, sizeof(u32));

    const auto& texturing = regs.texturing;
    u32* tex = key.texturing.data();
    std::memcpy(tex + 0, &texturing.main_config, sizeof(u32));
    std::memcpy(tex + 1, &texturing.texture0, 5 * sizeof(u32));
    std::memcpy(tex + 6, &texturing.texture0_format, sizeof(u32));
    std::memcpy(tex + 7, &texturing.texture1, 5 * sizeof(u32));
    std::memcpy(tex + 12, &texturing.texture1_format, sizeof(u32));
    std::memcpy(tex + 13, &texturing.texture2, 5 * sizeof(u32));
    std::memcpy(tex + 18, &texturing.texture2_format, sizeof(u32));

    key.using_fb = (using_color_fb ? 1u : 0u) | (using_depth_fb ? 2u : 0u);
    return key;
}

bool RasterizerOpenGL::Draw(bool accelerate, bool is_indexed) {
    MICROPROFILE_SCOPE(OpenGL_Drawing);
    const DebugScope scope(runtime, Common::Vec4f{}, "RasterizerOpenGL::Draw");

    SyncDrawState();

    const bool shadow_rendering = regs.framebuffer.IsShadowRendering();
    const bool has_stencil = regs.framebuffer.HasStencil();

    const bool write_color_fb = shadow_rendering || state.color_mask.red_enabled == GL_TRUE ||
                                state.color_mask.green_enabled == GL_TRUE ||
                                state.color_mask.blue_enabled == GL_TRUE ||
                                state.color_mask.alpha_enabled == GL_TRUE;

    const bool write_depth_fb =
        (state.depth.test_enabled && state.depth.write_mask == GL_TRUE) ||
        (has_stencil && state.stencil.test_enabled && state.stencil.write_mask != 0);

    const bool using_color_fb =
        regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress() != 0 && write_color_fb;
    const bool using_depth_fb =
        !shadow_rendering && regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress() != 0 &&
        (write_depth_fb || regs.framebuffer.output_merger.depth_test_enable != 0 ||
         (has_stencil && state.stencil.test_enabled));

    // [FB-NULL] LEGO Harry Potter 5-7 / LEGO SW Clone Wars III render into a
    // framebuffer with NO attachments (GL_FRAMEBUFFER_INCOMPLETE_MISSING_
    // ATTACHMENT), so every draw raises GL_INVALID_FRAMEBUFFER_OPERATION.
    // A draw with all writes masked off is a legitimate no-op; a draw with a
    // ZERO color-buffer address is not. Log which one this is, plus the
    // register state that decided it. Capped: fires only in the null case.
    // ([FB-ADDR] / [FB-STAT] removed 2026-07-25: ~17,000 LOG_CRITICAL calls
    // per session from this per-draw path — the "distinct address" dedupe
    // never fired because the colour-buffer address alternates every draw.
    // The LEGO question is solved via DISABLE_GPU_TIMING_SIM.)

    if (!using_color_fb && !using_depth_fb) {
        static u32 fb_null_count = 0;
        if (fb_null_count++ < 12) {
            LOG_CRITICAL(Render_OpenGL,
                         "[FB-NULL] #{} color_addr={:#010x} depth_addr={:#010x} | write_color={} "
                         "write_depth={} | mask rgba={}{}{}{} | depth_test={} depth_write={} "
                         "stencil(has={} test={} mask={:#x}) shadow={}",
                         fb_null_count, regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress(),
                         regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress(),
                         write_color_fb, write_depth_fb,
                         state.color_mask.red_enabled == GL_TRUE ? 1 : 0,
                         state.color_mask.green_enabled == GL_TRUE ? 1 : 0,
                         state.color_mask.blue_enabled == GL_TRUE ? 1 : 0,
                         state.color_mask.alpha_enabled == GL_TRUE ? 1 : 0, state.depth.test_enabled,
                         state.depth.write_mask == GL_TRUE, has_stencil, state.stencil.test_enabled,
                         state.stencil.write_mask, shadow_rendering);
        }
    }

    // DRAW_LOOKUP_REUSE (title-gated): see the HackType comment. When the registers the
    // framebuffer and texture lookups read match the last full lookup, and the rasterizer cache
    // has not changed since that lookup began, the lookups and the framebuffer invalidation
    // would be no-ops returning the same surfaces, so restore their bindings instead.
    const bool lookup_reuse = Common::Hacks::g_draw_lookup_reuse.load(std::memory_order_relaxed);
    DrawLookupKey lookup_key{};
    u64 lookup_generation = 0;
    if (lookup_reuse) {
        lookup_key = MakeDrawLookupKey(using_color_fb, using_depth_fb);
        lookup_generation = res_cache.MutationGeneration();
        if (draw_reuse.armed && !shadow_rendering && draw_reuse.generation == lookup_generation &&
            draw_reuse.key == lookup_key) {
            state.draw.draw_framebuffer = draw_reuse.draw_framebuffer;
            state.viewport = draw_reuse.viewport;
            state.scissor = draw_reuse.scissor;
            state.texture_units = draw_reuse.texture_units;
            state.color_buffer.texture_2d = draw_reuse.color_buffer_texture;
            state.image_shadow_texture = draw_reuse.image_shadow_texture;
            user_config = draw_reuse.user_config;
            state.Apply();
            return DrawBatch(accelerate, is_indexed, false);
        }
        draw_reuse.armed = false;
        draw_had_feedback_loop = false;
    }

    const auto fb_helper = res_cache.GetFramebufferSurfaces(using_color_fb, using_depth_fb);
    const Framebuffer* framebuffer = fb_helper.Framebuffer();
    if (!framebuffer->color_id && framebuffer->shadow_rendering) {
        return true;
    }

    // Bind the framebuffer surfaces
    if (shadow_rendering) {
        state.image_shadow_buffer = framebuffer->Attachment(SurfaceType::Color);
    }
    state.draw.draw_framebuffer = framebuffer->Handle();

    // Sync the viewport
    const auto viewport = fb_helper.Viewport();
    state.viewport.x = static_cast<GLint>(viewport.x);
    state.viewport.y = static_cast<GLint>(viewport.y);
    state.viewport.width = static_cast<GLsizei>(viewport.width);
    state.viewport.height = static_cast<GLsizei>(viewport.height);

    // Viewport can have negative offsets or larger dimensions than our framebuffer sub-rect.
    // Enable scissor test to prevent drawing outside of the framebuffer region
    const auto draw_rect = fb_helper.DrawRect();
    state.scissor.enabled = true;
    state.scissor.x = draw_rect.left;
    state.scissor.y = draw_rect.bottom;
    state.scissor.width = draw_rect.GetWidth();
    state.scissor.height = draw_rect.GetHeight();

    // Update scissor uniforms
    const auto [scissor_x1, scissor_y2, scissor_x2, scissor_y1] = fb_helper.Scissor();
    if (fs_data.scissor_x1 != scissor_x1 || fs_data.scissor_x2 != scissor_x2 ||
        fs_data.scissor_y1 != scissor_y1 || fs_data.scissor_y2 != scissor_y2) {

        fs_data.scissor_x1 = scissor_x1;
        fs_data.scissor_x2 = scissor_x2;
        fs_data.scissor_y1 = scissor_y1;
        fs_data.scissor_y2 = scissor_y2;
        fs_data_dirty = true;
    }

    // Sync and bind the texture surfaces
    SyncTextureUnits(framebuffer);
    state.Apply();

    if (lookup_reuse) {
        // Never reuse a draw whose lookups have per-draw side effects: shadow rendering, a
        // feedback-loop copy of the colour attachment, cube/shadow texture 0 (face copies,
        // surface flags) or a custom material.
        using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
        const auto tex0_type = regs.texturing.texture0.type.Value();
        const bool tex0_special =
            regs.texturing.main_config.texture0_enable.Value() != 0 &&
            (tex0_type == TextureType::TextureCube || tex0_type == TextureType::Shadow2D ||
             tex0_type == TextureType::ShadowCube);
        if (!shadow_rendering && !draw_had_feedback_loop && !tex0_special &&
            user_config.use_custom_normal.Value() == 0) {
            draw_reuse.armed = true;
            draw_reuse.generation = lookup_generation;
            draw_reuse.key = lookup_key;
            draw_reuse.draw_framebuffer = state.draw.draw_framebuffer;
            draw_reuse.viewport = state.viewport;
            draw_reuse.scissor = state.scissor;
            draw_reuse.texture_units = state.texture_units;
            draw_reuse.color_buffer_texture = state.color_buffer.texture_2d;
            draw_reuse.image_shadow_texture = state.image_shadow_texture;
            draw_reuse.user_config = user_config;
        }
    }

    return DrawBatch(accelerate, is_indexed, shadow_rendering);
}

bool RasterizerOpenGL::DrawBatch(bool accelerate, bool is_indexed, bool shadow_rendering) {
    // Sync and bind the shader
    curr_shader_manager->UseFragmentShader(regs, user_config);

    // Sync the LUTs within the texture buffer
    SyncAndUploadLUTs();
    SyncAndUploadLUTsLF();

    // Sync the uniform data
    UploadUniforms(accelerate);

    // Draw the vertex batch
    bool succeeded = true;
    if (accelerate) {
        succeeded = AccelerateDrawBatchInternal(is_indexed);
    } else {
        state.draw.vertex_array = sw_vao.handle;
        state.draw.vertex_buffer = vertex_buffer.GetHandle();
        curr_shader_manager->UseTrivialVertexShader();
        curr_shader_manager->UseTrivialGeometryShader();
        curr_shader_manager->ApplyTo(state, accurate_mul);
        state.Apply();

        std::size_t max_vertices = 3 * (VERTEX_BUFFER_SIZE / (3 * sizeof(HardwareVertex)));
        for (std::size_t base_vertex = 0; base_vertex < vertex_batch.size();
             base_vertex += max_vertices) {
            const std::size_t vertices = std::min(max_vertices, vertex_batch.size() - base_vertex);
            const std::size_t vertex_size = vertices * sizeof(HardwareVertex);

            const auto [vbo, offset, _] = vertex_buffer.Map(vertex_size, sizeof(HardwareVertex));
            std::memcpy(vbo, vertex_batch.data() + base_vertex, vertex_size);
            vertex_buffer.Unmap(vertex_size);

            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(offset / sizeof(HardwareVertex)),
                         static_cast<GLsizei>(vertices));
        }
    }

    vertex_batch.clear();

    if (shadow_rendering) {
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                        GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    }

    return succeeded;
}

void RasterizerOpenGL::SyncTextureUnits(const Framebuffer* framebuffer) {
    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;

    // Reset transient draw state
    state.color_buffer.texture_2d = 0;
    user_config = {};

    const auto pica_textures = regs.texturing.GetTextures();
    for (u32 texture_index = 0; texture_index < pica_textures.size(); ++texture_index) {
        const auto& texture = pica_textures[texture_index];

        // If the texture unit is disabled unbind the corresponding gl unit
        if (!texture.enabled) {
            switch (texture.config.type.Value()) {
            case TextureType::TextureCube:
            case TextureType::ShadowCube: {
                state.texture_units[texture_index].texture_2d =
                    res_cache.GetSurface(VideoCore::NULL_SURFACE_CUBE_ID).Handle();
                state.texture_units[texture_index].target = GL_TEXTURE_CUBE_MAP;
                break;
            }
            default: {
                state.texture_units[texture_index].texture_2d =
                    res_cache.GetSurface(VideoCore::NULL_SURFACE_ID).Handle();
                state.texture_units[texture_index].target = GL_TEXTURE_2D;
                break;
            }
            }
            continue;
        }

        // Handle special tex0 configurations
        if (texture_index == 0) {
            switch (texture.config.type.Value()) {
            case TextureType::Shadow2D: {
                Surface& surface = res_cache.GetTextureSurface(texture);
                surface.flags |= VideoCore::SurfaceFlagBits::ShadowSource;
                state.image_shadow_texture_px = surface.Handle();
                continue;
            }
            case TextureType::ShadowCube: {
                BindShadowCube(texture);
                continue;
            }
            case TextureType::TextureCube: {
                BindTextureCube(texture);
                continue;
            }
            default:
                UnbindSpecial();
            }
        }

        // Sync texture unit sampler
        Sampler& sampler = res_cache.GetSampler(texture.config);
        state.texture_units[texture_index].sampler = sampler.Handle();

        // Bind the texture provided by the rasterizer cache
        Surface& surface = res_cache.GetTextureSurface(texture);
        if (!IsFeedbackLoop(texture_index, framebuffer, surface)) {
            BindMaterial(texture_index, surface);
            state.texture_units[texture_index].texture_2d = surface.Handle();
            state.texture_units[texture_index].target = GL_TEXTURE_2D;
        }
    }

    if (emulate_minmax_blend && !driver.HasShaderFramebufferFetch()) {
        state.color_buffer.texture_2d = framebuffer->Attachment(SurfaceType::Color);
    }
}

void RasterizerOpenGL::BindShadowCube(const Pica::TexturingRegs::FullTextureConfig& texture) {
    using CubeFace = Pica::TexturingRegs::CubeFace;
    auto info = Pica::Texture::TextureInfo::FromPicaRegister(texture.config, texture.format);
    constexpr std::array faces = {
        CubeFace::PositiveX, CubeFace::NegativeX, CubeFace::PositiveY,
        CubeFace::NegativeY, CubeFace::PositiveZ, CubeFace::NegativeZ,
    };

    for (CubeFace face : faces) {
        const u32 binding = static_cast<u32>(face);
        info.physical_address = regs.texturing.GetCubePhysicalAddress(face);

        VideoCore::SurfaceId surface_id = res_cache.GetTextureSurface(info);
        Surface& surface = res_cache.GetSurface(surface_id);
        surface.flags |= VideoCore::SurfaceFlagBits::ShadowSource;
        state.image_shadow_texture[binding] = surface.Handle();
    }
}

void RasterizerOpenGL::BindTextureCube(const Pica::TexturingRegs::FullTextureConfig& texture) {
    using CubeFace = Pica::TexturingRegs::CubeFace;
    const VideoCore::TextureCubeConfig config = {
        .px = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX),
        .nx = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX),
        .py = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY),
        .ny = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY),
        .pz = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ),
        .nz = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ),
        .width = texture.config.width,
        .levels = texture.config.lod.max_level + 1,
        .format = texture.format,
    };

    Surface& surface = res_cache.GetTextureCube(config);
    Sampler& sampler = res_cache.GetSampler(texture.config);
    state.texture_units[0].target = GL_TEXTURE_CUBE_MAP;
    state.texture_units[0].texture_2d = surface.Handle();
    state.texture_units[0].sampler = sampler.Handle();
}

void RasterizerOpenGL::BindMaterial(u32 texture_index, Surface& surface) {
    if (!surface.IsCustom()) {
        return;
    }

    const GLuint sampler = state.texture_units[texture_index].sampler;
    if (surface.HasNormalMap()) {
        if (regs.lighting.disable) {
            LOG_WARNING(Render_OpenGL, "Custom normal map used but scene has no light enabled");
        }
        glActiveTexture(TextureUnits::TextureNormalMap.Enum());
        glBindTexture(GL_TEXTURE_2D, surface.Handle(2));
        glBindSampler(TextureUnits::TextureNormalMap.id, sampler);
        user_config.use_custom_normal.Assign(1);
    }
}

bool RasterizerOpenGL::IsFeedbackLoop(u32 texture_index, const Framebuffer* framebuffer,
                                      Surface& surface) {
    const GLuint color_attachment = framebuffer->Attachment(SurfaceType::Color);
    const bool is_feedback_loop = color_attachment == surface.Handle();
    if (!is_feedback_loop) {
        return false;
    }

    draw_had_feedback_loop = true;
    state.texture_units[texture_index].texture_2d = surface.CopyHandle();
    return true;
}

void RasterizerOpenGL::UnbindSpecial() {
    state.texture_units[0].texture_2d = 0;
    state.texture_units[0].target = GL_TEXTURE_2D;
    state.image_shadow_texture_px = 0;
    state.image_shadow_texture_nx = 0;
    state.image_shadow_texture_py = 0;
    state.image_shadow_texture_ny = 0;
    state.image_shadow_texture_pz = 0;
    state.image_shadow_texture_nz = 0;
    state.image_shadow_buffer = 0;
}

void RasterizerOpenGL::FlushAll() {
    FlushDrawMerge();
    res_cache.FlushAll();
}

void RasterizerOpenGL::FlushRegion(PAddr addr, u32 size) {
    FlushDrawMerge();
    res_cache.FlushRegion(addr, size);
}

void RasterizerOpenGL::InvalidateRegion(PAddr addr, u32 size) {
    FlushDrawMerge();
    res_cache.InvalidateRegion(addr, size);
}

void RasterizerOpenGL::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    FlushDrawMerge();
    res_cache.FlushRegion(addr, size);
    res_cache.InvalidateRegion(addr, size);
}

void RasterizerOpenGL::ClearAll(bool flush) {
    FlushDrawMerge();
    res_cache.ClearAll(flush);
}

bool RasterizerOpenGL::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
    FlushDrawMerge();
    return res_cache.AccelerateDisplayTransfer(config);
}

bool RasterizerOpenGL::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    FlushDrawMerge();
    return res_cache.AccelerateTextureCopy(config);
}

bool RasterizerOpenGL::AccelerateFill(const Pica::MemoryFillConfig& config) {
    FlushDrawMerge();
    return res_cache.AccelerateFill(config);
}

bool RasterizerOpenGL::AccelerateDisplay(const Pica::FramebufferConfig& config,
                                         PAddr framebuffer_addr, u32 pixel_stride,
                                         ScreenInfo& screen_info) {
    FlushDrawMerge();
    if (framebuffer_addr == 0) {
        return false;
    }
    MICROPROFILE_SCOPE(OpenGL_Display);

    VideoCore::SurfaceParams src_params;
    src_params.addr = framebuffer_addr;
    src_params.width = std::min(config.width.Value(), pixel_stride);
    src_params.height = config.height;
    src_params.stride = pixel_stride;
    src_params.is_tiled = false;
    src_params.pixel_format = VideoCore::PixelFormatFromGPUPixelFormat(config.color_format);
    src_params.UpdateParams();

    const auto [src_surface_id, src_rect] =
        res_cache.GetSurfaceSubRect(src_params, VideoCore::ScaleMatch::Ignore, true);
    if (!src_surface_id) {
        return false;
    }

    const DebugScope scope{runtime,
                           Common::Vec4f{0.f, 1.f, 1.f, 1.f},
                           "RasterizerOpenGL::AccelerateDisplay ({}x{} {} at {:#X})",
                           src_params.width,
                           src_params.height,
                           VideoCore::PixelFormatAsString(src_params.pixel_format),
                           src_params.addr};

    const Surface& src_surface = res_cache.GetSurface(src_surface_id);
    const u32 scaled_width = src_surface.GetScaledWidth();
    const u32 scaled_height = src_surface.GetScaledHeight();

    screen_info.display_texcoords = Common::Rectangle<float>(
        (float)src_rect.bottom / (float)scaled_height, (float)src_rect.left / (float)scaled_width,
        (float)src_rect.top / (float)scaled_height, (float)src_rect.right / (float)scaled_width);

    screen_info.display_texture = src_surface.Handle();

    // FB_CONFIG_DIAG (log only): presentation resolution + CONTENT probe.
    // The geometry chain is proven identical to Citra 2104, so the shift
    // must be in the pixels themselves. Flush the framebuffer back to
    // guest RAM and report where the image actually starts: the offset of
    // the first non-black pixel, and the first non-black pixel of a row
    // near the middle. A displaced image reports a different offset — the
    // difference IS the shift, in bytes.
    if (Common::Hacks::g_fb_config_diag.load(std::memory_order_relaxed)) {
        static u32 pres_tick = 0;
        if ((pres_tick++ % 241) < 3) {
            LOG_INFO(HW_GPU,
                     "[PRESENT] fb=0x{:08X} -> surf=0x{:08X} {}x{} rect=({},{})-({},{}) "
                     "scaled={}x{}",
                     framebuffer_addr, src_surface.addr, src_surface.width, src_surface.height,
                     src_rect.left, src_rect.bottom, src_rect.right, src_rect.top, scaled_width,
                     scaled_height);

            const u32 bpp = Pica::BytesPerPixel(config.color_format);
            const u32 row_bytes = pixel_stride * bpp;
            const u32 total = row_bytes * config.height;
            res_cache.FlushRegion(framebuffer_addr, total);
            const u8* px = memory.GetPhysicalPointer(framebuffer_addr);
            if (px && total) {
                u32 first_nz = 0xFFFFFFFF;
                for (u32 i = 0; i < total; ++i) {
                    if (px[i] != 0) {
                        first_nz = i;
                        break;
                    }
                }
                const u32 mid_row = config.height / 2;
                u32 mid_nz = 0xFFFFFFFF;
                for (u32 i = 0; i < row_bytes; ++i) {
                    if (px[mid_row * row_bytes + i] != 0) {
                        mid_nz = i;
                        break;
                    }
                }
                u64 sum = 0;
                for (u32 i = 0; i < total; i += 97) {
                    sum = sum * 131 + px[i];
                }
                LOG_INFO(HW_GPU,
                         "[CONTENT] fb=0x{:08X} bpp={} row_bytes={} total={} first_nonzero={} "
                         "(row {} col_px {}) midrow_nonzero_byte={} hash=0x{:016X}",
                         framebuffer_addr, bpp, row_bytes, total, first_nz,
                         first_nz == 0xFFFFFFFF ? 0 : first_nz / row_bytes,
                         first_nz == 0xFFFFFFFF ? 0 : (first_nz % row_bytes) / bpp, mid_nz, sum);
            }
        }
    }

    return true;
}

void RasterizerOpenGL::SyncAndUploadLUTsLF() {
    constexpr std::size_t max_size =
        sizeof(Common::Vec2f) * 256 * Pica::LightingRegs::NumLightingSampler +
        sizeof(Common::Vec2f) * 128; // fog

    if (!pica.lighting.lut_dirty && !pica.fog.lut_dirty) {
        return;
    }

    std::size_t bytes_used = 0;
    glBindBuffer(GL_TEXTURE_BUFFER, texture_lf_buffer.GetHandle());
    const auto [buffer, offset, invalidate] =
        texture_lf_buffer.Map(max_size, sizeof(Common::Vec4f));

    if (invalidate) {
        pica.lighting.lut_dirty = pica.lighting.LutAllDirty;
        pica.fog.lut_dirty = true;
    }

    // Sync the lighting luts
    while (pica.lighting.lut_dirty) {
        const u32 index = std::countr_zero(pica.lighting.lut_dirty);
        pica.lighting.lut_dirty &= ~(1 << index);

        Common::Vec2f* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
        const auto& source_lut = pica.lighting.luts[index];
        for (u32 i = 0; i < source_lut.size(); i++) {
            new_data[i] = {source_lut[i].ToFloat(), source_lut[i].DiffToFloat()};
        }
        fs_data.lighting_lut_offset[index / 4][index % 4] =
            static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
        fs_data_dirty = true;
        bytes_used += source_lut.size() * sizeof(Common::Vec2f);
    }

    // Sync the fog lut
    if (pica.fog.lut_dirty) {
        Common::Vec2f* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
        for (u32 i = 0; i < pica.fog.lut.size(); i++) {
            new_data[i] = {pica.fog.lut[i].ToFloat(), pica.fog.lut[i].DiffToFloat()};
        }
        fs_data.fog_lut_offset = static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
        fs_data_dirty = true;
        bytes_used += pica.fog.lut.size() * sizeof(Common::Vec2f);
        pica.fog.lut_dirty = false;
    }

    texture_lf_buffer.Unmap(bytes_used);
}

void RasterizerOpenGL::SyncAndUploadLUTs() {
    constexpr std::size_t max_size =
        sizeof(Common::Vec2f) * 128 * 3 + // proctex: noise + color + alpha
        sizeof(Common::Vec4f) * 256 +     // proctex
        sizeof(Common::Vec4f) * 256;      // proctex diff

    if (!pica.proctex.table_dirty) {
        return;
    }

    std::size_t bytes_used = 0;
    glBindBuffer(GL_TEXTURE_BUFFER, texture_buffer.GetHandle());
    const auto [buffer, offset, invalidate] = texture_buffer.Map(max_size, sizeof(Common::Vec4f));

    if (invalidate) {
        pica.proctex.table_dirty = pica.proctex.TableAllDirty;
    }

    // helper function for SyncProcTexNoiseLUT/ColorMap/AlphaMap
    const auto sync_proc_tex_value_lut = [&](const auto& lut, GLint& lut_offset) {
        Common::Vec2f* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
        for (u32 i = 0; i < lut.size(); i++) {
            new_data[i] = {lut[i].ToFloat(), lut[i].DiffToFloat()};
        }
        lut_offset = static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
        fs_data_dirty = true;
        bytes_used += lut.size() * sizeof(Common::Vec2f);
    };

    // Sync the proctex noise lut
    if (pica.proctex.noise_lut_dirty) {
        sync_proc_tex_value_lut(pica.proctex.noise_table, fs_data.proctex_noise_lut_offset);
    }

    // Sync the proctex color map
    if (pica.proctex.color_map_dirty) {
        sync_proc_tex_value_lut(pica.proctex.color_map_table, fs_data.proctex_color_map_offset);
    }

    // Sync the proctex alpha map
    if (pica.proctex.alpha_map_dirty) {
        sync_proc_tex_value_lut(pica.proctex.alpha_map_table, fs_data.proctex_alpha_map_offset);
    }

    // Sync the proctex lut
    if (pica.proctex.lut_dirty) {
        Common::Vec4f* new_data = reinterpret_cast<Common::Vec4f*>(buffer + bytes_used);
        for (u32 i = 0; i < pica.proctex.color_table.size(); i++) {
            new_data[i] = pica.proctex.color_table[i].ToVector() / 255.0f;
        }
        fs_data.proctex_lut_offset =
            static_cast<int>((offset + bytes_used) / sizeof(Common::Vec4f));
        fs_data_dirty = true;
        bytes_used += pica.proctex.color_table.size() * sizeof(Common::Vec4f);
    }

    // Sync the proctex difference lut
    if (pica.proctex.diff_lut_dirty) {
        Common::Vec4f* new_data = reinterpret_cast<Common::Vec4f*>(buffer + bytes_used);
        for (u32 i = 0; i < pica.proctex.color_diff_table.size(); i++) {
            new_data[i] = pica.proctex.color_diff_table[i].ToVector() / 255.0f;
        }
        fs_data.proctex_diff_lut_offset =
            static_cast<int>((offset + bytes_used) / sizeof(Common::Vec4f));
        fs_data_dirty = true;
        bytes_used += pica.proctex.color_diff_table.size() * sizeof(Common::Vec4f);
    }

    pica.proctex.table_dirty = 0;

    texture_buffer.Unmap(bytes_used);
}

void RasterizerOpenGL::UploadUniforms(bool accelerate_draw) {
    // glBindBufferRange also changes the generic buffer binding point, so we sync the state first.
    state.draw.uniform_buffer = uniform_buffer.GetHandle();
    state.Apply();

    const bool sync_vs_pica = accelerate_draw && pica.vs_setup.uniforms_dirty;
    if (!sync_vs_pica && !vs_data_dirty && !fs_data_dirty) {
        return;
    }

    std::size_t uniform_size =
        uniform_size_aligned_vs_pica + uniform_size_aligned_vs + uniform_size_aligned_fs;
    std::size_t used_bytes = 0;

    const auto [uniforms, offset, invalidate] =
        uniform_buffer.Map(uniform_size, uniform_buffer_alignment);

    if (vs_data_dirty || invalidate) {
        std::memcpy(uniforms + used_bytes, &vs_data, sizeof(vs_data));
        glBindBufferRange(GL_UNIFORM_BUFFER, UniformBindings::VSData, uniform_buffer.GetHandle(),
                          offset + used_bytes, sizeof(vs_data));
        vs_data_dirty = false;
        used_bytes += uniform_size_aligned_vs;
    }

    if (fs_data_dirty || invalidate) {
        std::memcpy(uniforms + used_bytes, &fs_data, sizeof(fs_data));
        glBindBufferRange(GL_UNIFORM_BUFFER, UniformBindings::FSData, uniform_buffer.GetHandle(),
                          offset + used_bytes, sizeof(fs_data));
        fs_data_dirty = false;
        used_bytes += uniform_size_aligned_fs;
    }

    if (sync_vs_pica || invalidate) {
        VSPicaUniformData vs_uniforms;
        vs_uniforms.SetFromRegs(pica.vs_setup);
        std::memcpy(uniforms + used_bytes, &vs_uniforms, sizeof(vs_uniforms));
        glBindBufferRange(GL_UNIFORM_BUFFER, UniformBindings::VSPicaData,
                          uniform_buffer.GetHandle(), offset + used_bytes, sizeof(vs_uniforms));
        pica.vs_setup.uniforms_dirty = false;
        used_bytes += uniform_size_aligned_vs_pica;
    }

    uniform_buffer.Unmap(used_bytes);
}

} // namespace OpenGL
