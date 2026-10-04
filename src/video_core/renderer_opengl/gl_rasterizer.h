// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "video_core/rasterizer_accelerated.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_stream_buffer.h"
#include "video_core/renderer_opengl/gl_texture_runtime.h"

namespace VideoCore {
class RendererBase;
}

namespace VideoCore {
class CustomTexManager;
}

namespace Pica {
struct DisplayTransferConfig;
struct MemoryFillConfig;
struct FramebufferConfig;
} // namespace Pica

namespace OpenGL {

struct ScreenInfo;

class Driver;
class ShaderProgramManager;

class RasterizerOpenGL : public VideoCore::RasterizerAccelerated {
public:
    explicit RasterizerOpenGL(Memory::MemorySystem& memory, Pica::PicaCore& pica,
                              VideoCore::CustomTexManager& custom_tex_manager,
                              VideoCore::RendererBase& renderer, Driver& driver);
    ~RasterizerOpenGL() override;

    void TickFrame();
    void LoadDefaultDiskResources(const std::atomic_bool& stop_loading,
                                  const VideoCore::DiskResourceLoadCallback& callback) override;
    void SwitchDiskResources(u64 title_id) override;

    void DrawTriangles() override;
    void FlushAll() override;
    void FlushRegion(PAddr addr, u32 size) override;
    void InvalidateRegion(PAddr addr, u32 size) override;
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override;
    void ClearAll(bool flush) override;
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateFill(const Pica::MemoryFillConfig& config) override;
    bool AccelerateDisplay(const Pica::FramebufferConfig& config, PAddr framebuffer_addr,
                           u32 pixel_stride, ScreenInfo& screen_info);
    bool AccelerateDrawBatch(bool is_indexed) override;

    /// DRAW_LOOKUP_REUSE: issue the merged draws still pending (no-op when none are).
    void FlushDrawMerge();

private:
    /// DRAW_LOOKUP_REUSE: append this accelerated draw to the pending merged draws when nothing
    /// but its vertices differs from them; false when it needs the full draw path.
    bool TryAppendDrawMerge(bool is_indexed);

    /// Syncs pipeline state from PICA registers
    void SyncDrawState();

    /// Syncs and uploads the lighting, fog and proctex LUTs
    void SyncAndUploadLUTs();
    void SyncAndUploadLUTsLF();

    /// Syncs all enabled PICA texture units
    void SyncTextureUnits(const Framebuffer* framebuffer);

    /// Binds the PICA shadow cube required for shadow mapping
    void BindShadowCube(const Pica::TexturingRegs::FullTextureConfig& texture);

    /// Binds a texture cube to texture unit 0
    void BindTextureCube(const Pica::TexturingRegs::FullTextureConfig& texture);

    /// Makes a temporary copy of the framebuffer if a feedback loop is detected
    bool IsFeedbackLoop(u32 texture_index, const Framebuffer* framebuffer, Surface& surface);

    /// Unbinds all special texture unit 0 texture configurations
    void UnbindSpecial();

    /// Binds the custom material referenced by surface if it exists.
    void BindMaterial(u32 texture_index, Surface& surface);

    /// Upload the uniform blocks to the uniform buffer object
    void UploadUniforms(bool accelerate_draw);

    /// Generic draw function for DrawTriangles and AccelerateDrawBatch
    bool Draw(bool accelerate, bool is_indexed);

    /// Shader, LUT and uniform sync plus the draw itself; the second half of Draw
    bool DrawBatch(bool accelerate, bool is_indexed, bool shadow_rendering);

    /// DRAW_LOOKUP_REUSE: the PICA registers that GetFramebufferSurfaces and SyncTextureUnits
    /// read (framebuffer config, viewport, scissor, texture units 0-2), plus the two
    /// framebuffer-use flags Draw passes in.
    struct DrawLookupKey {
        std::array<u32, 16> framebuffer{};
        std::array<u32, 6> viewport_scissor{};
        std::array<u32, 19> texturing{};
        u32 using_fb{};
        bool operator==(const DrawLookupKey&) const = default;
    };
    DrawLookupKey MakeDrawLookupKey(bool using_color_fb, bool using_depth_fb) const;

    /// Internal implementation for AccelerateDrawBatch
    bool AccelerateDrawBatchInternal(bool is_indexed);

    /// Setup vertex array for AccelerateDrawBatch
    void SetupVertexArray(u8* array_ptr, GLintptr buffer_offset, GLuint vs_input_index_min,
                          GLuint vs_input_index_max);

    /// DRAW_LOOKUP_REUSE: index of the only attribute loader with data, or -1 when there are
    /// none or several.
    s32 SingleAttributeLoader() const;

    /// DRAW_LOOKUP_REUSE: SetupVertexArray for a single interleaved loader, with the attribute
    /// pointers at buffer offset 0 so a draw addresses its vertices through the base vertex.
    /// Re-specifies the pointers and default attributes only when they differ from the last
    /// draw's.
    void SetupVertexArrayZeroBase(u32 loader_index, u8* array_ptr, GLuint vs_input_index_min,
                                  GLuint vs_input_index_max);

    /// Setup vertex shader for AccelerateDrawBatch
    bool SetupVertexShader();

    /// Setup geometry shader for AccelerateDrawBatch
    bool SetupGeometryShader();

private:
    Driver& driver;
    OpenGLState state;
    Frontend::EmuWindow& render_window;
    std::vector<std::shared_ptr<ShaderProgramManager>> shader_managers;
    std::shared_ptr<ShaderProgramManager> curr_shader_manager{};
    TextureRuntime runtime;
    RasterizerCache res_cache;

    OGLVertexArray sw_vao; // VAO for software shader draw
    OGLVertexArray hw_vao; // VAO for hardware shader / accelerate draw
    std::array<bool, 16> hw_vao_enabled_attributes{};

    OGLStreamBuffer vertex_buffer;
    OGLStreamBuffer uniform_buffer;
    OGLStreamBuffer index_buffer;
    OGLStreamBuffer texture_buffer;
    OGLStreamBuffer texture_lf_buffer;
    GLint uniform_buffer_alignment;
    std::size_t uniform_size_aligned_vs_pica;
    std::size_t uniform_size_aligned_vs;
    std::size_t uniform_size_aligned_fs;

    OGLTexture texture_buffer_lut_lf;
    OGLTexture texture_buffer_lut_rg;
    OGLTexture texture_buffer_lut_rgba;
    bool emulate_minmax_blend{};

    /// DRAW_LOOKUP_REUSE: the last full lookup's key, the cache generation it started at, and
    /// the bindings it produced. Reused while the key and generation both still match.
    struct DrawLookupReuse {
        bool armed{};
        u64 generation{};
        DrawLookupKey key{};
        GLuint draw_framebuffer{};
        decltype(OpenGLState::viewport) viewport{};
        decltype(OpenGLState::scissor) scissor{};
        std::array<OpenGLState::TextureUnit, 3> texture_units{};
        GLuint color_buffer_texture{};
        std::array<GLuint, 6> image_shadow_texture{};
        Pica::Shader::UserConfig user_config{};
    } draw_reuse;
    /// Set by IsFeedbackLoop when a bound texture is the colour attachment (never reused).
    bool draw_had_feedback_loop{};

    /// DRAW_LOOKUP_REUSE: the attribute pointers and default attributes SetupVertexArrayZeroBase
    /// last specified. Cleared whenever SetupVertexArray re-points the attributes.
    struct ZeroBaseAttrib {
        u32 input_reg{};
        GLint size{};
        GLenum type{};
        GLsizei stride{};
        u32 offset{};
        bool operator==(const ZeroBaseAttrib&) const = default;
    };
    struct ZeroBaseLayout {
        bool valid{};
        u32 count{};
        std::array<ZeroBaseAttrib, 12> attribs{};
        std::array<bool, 16> default_known{};
        std::array<Common::Vec4f, 16> default_value{};
    } zero_base_layout;

    /// DRAW_LOOKUP_REUSE: consecutive non-indexed draws whose GL state is identical (no render
    /// register, uniform, shader, LUT or cache change since the first) are held here and issued
    /// as one glMultiDrawArrays. Every other rasterizer entry point flushes them first.
    struct DrawMerge {
        GLenum mode{};
        s32 loader{-1};
        u32 stride{};
        u64 pica_breaks{};
        u64 cache_generation{};
        std::vector<GLint> firsts;
        std::vector<GLsizei> counts;
    } draw_merge;
};

} // namespace OpenGL
