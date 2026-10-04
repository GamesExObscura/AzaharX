// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// High precision may or may not be supported in GLES3. If it isn't, use medium precision instead.
static constexpr char fragment_shader_precision_OES[] = R"(
#if GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp int;
precision highp float;
precision highp samplerBuffer;
precision highp uimage2D;
#else
precision mediump int;
precision mediump float;
precision mediump samplerBuffer;
precision mediump uimage2D;
#endif // GL_FRAGMENT_PRECISION_HIGH
#endif
)";

namespace Pica {
struct ShaderSetup;
}

namespace Pica::Shader::Generator {
struct PicaVSConfig;
struct ExtraVSConfig;
struct PicaFixedGSConfig;
struct ExtraFixedGSConfig;
} // namespace Pica::Shader::Generator

namespace Pica::Shader::Generator::GLSL {

/**
 * Generates the GLSL vertex shader program source code that accepts vertices from software shader
 * and directly passes them to the fragment shader.
 * @returns String of the shader source code
 */
std::string GenerateTrivialVertexShader(bool use_clip_planes, bool separable_shader);

/**
 * Generates the GLSL vertex shader program source code for the given VS program
 * @returns String of the shader source code; empty on failure
 */
std::string GenerateVertexShader(const Pica::ShaderSetup& setup, const PicaVSConfig& config,
                                 const ExtraVSConfig& extra);

/**
 * Generates the GLSL fixed geometry shader program source code for non-GS PICA pipeline
 * @returns String of the shader source code
 */
std::string GenerateFixedGeometryShader(const PicaFixedGSConfig& config,
                                        const ExtraFixedGSConfig& extra_config);

/**
 * PMG3D-only fix: when enabled, generated fragment shaders sample tex0 at
 * base mip (textureLod = 0) instead of the standard computed-LOD path.
 * PMG3D's intro logo textures have higher mip levels that aren't populated
 * in Azahar's upload path, so the computed-LOD sample lands on empty data
 * and returns (0,0,0,0) — turning the logos black even though the base mip
 * has valid content. Forcing LOD 0 reads only the base mip and recovers the
 * intended texture. Title-gated via HackType::PMG3D_FORCE_WHITE_AMBIENT
 * (the hack name is a legacy label — actual effect is LOD-0 sampling).
 */
void EnablePmg3dForceLod0(bool enable);

/**
 * PMG3D-only fix: when enabled, the VS / trivial-GS clip sites emit
 * `gl_ClipDistance[0] = 1.0` (always pass) instead of the normal
 * `-vtx_pos.z`. PMG3D's intro shader writes vs_out_attr0.z = f[93].y, which
 * produces a negative ClipDistance under PICA's z<=0 plane and would
 * otherwise discard every logo / UI / gameplay vertex. Relaxing the clip
 * lets those draws survive. The fixed-GS path is NOT affected (as of
 * 2026-07-04 it is always strict): the lit 3D meshes it renders — PMG3D's
 * building, host, and doors — draw correctly with natural clipping and
 * broke under the old whole-pipeline relax. Title-gated via
 * HackType::PMG3D_RELAX_CLIP_PLANE.
 */
void EnablePmg3dRelaxClipPlane(bool enable);
} // namespace Pica::Shader::Generator::GLSL
