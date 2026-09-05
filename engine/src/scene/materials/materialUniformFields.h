// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// THE per-draw material uniform block, declared once.
//
// This block used to be written out by hand in four places that had to stay in
// lockstep — the C++ struct, the MSL `MaterialData`, the GLSL block in
// forward-fragment-head, and a verbatim copy in forward.vert (MoltenVK
// miscompiles a UBO whose member list differs between stages). Adding a field
// meant editing all four, and a mismatch shifts every field after it, which
// shows up as silent corruption rather than a compile error.
//
// Now there is one list. The C++ struct expands from it, and the MSL and GLSL
// declarations are emitted from it — at runtime by ProgramLibrary, and at build
// time by tools/generate_vulkan_shader_bundle.py for the SPIR-V bundle.
//
// Field types are SHADER types (vec4 / float / uint); the C++ side maps vec4 to
// float[4] and uint to uint32_t. Scalars pack at 4-byte offsets in both MSL and
// std140, and every vec4 here is already 16-byte aligned by construction, so one
// layout serves both languages.
//
#pragma once

#include <cstdint>
#include <string>

// Shader type -> C++ storage. A vec4 is four floats (element type + dimension,
// since the dimension has to follow the member name in C++); scalars map directly.
#define VT_MATERIAL_CPP_vec4  float
#define VT_MATERIAL_CPP_float float
#define VT_MATERIAL_CPP_uint  uint32_t
#define VT_MATERIAL_DIM_vec4  [4]
#define VT_MATERIAL_DIM_float
#define VT_MATERIAL_DIM_uint

// X(shaderType, name, default...) — variadic so a braced initialiser's commas
// stay inside one argument.
#define VT_MATERIAL_UNIFORM_FIELDS(X) \
    X(vec4, baseColor, {1.0f, 1.0f, 1.0f, 1.0f}) \
    X(vec4, emissiveColor, {0.0f, 0.0f, 0.0f, 1.0f}) \
    X(uint, flags, 0u) \
    X(uint, occludeSpecularMode, SPECOCC_AO) \
    X(float, alphaCutoff, 0.5f) \
    X(float, metallicFactor, 0.0f) \
    X(float, roughnessFactor, 1.0f) \
    X(float, normalScale, 1.0f) \
    X(float, occlusionStrength, 1.0f) \
    X(float, occludeSpecularIntensity, 1.0f) \
    /* per-texture UV transforms as pre-computed 3×2 affine matrices. */ \
    /* Each pair of float[4] encodes one row of the matrix: */ \
    /* row0 = {cos(θ)*sx, -sin(θ)*sy, ox, 0} */ \
    /* row1 = {sin(θ)*sx,  cos(θ)*sy, 1-sy-oy, 0} */ \
    /* Identity: row0={1,0,0,0}, row1={0,1,0,0} */ \
    /* GPU applies: uv' = float2(dot(float3(uv,1), row0.xyz), dot(float3(uv,1), row1.xyz)) */ \
    X(vec4, baseColorTransform0, {1, 0, 0, 0}) \
    X(vec4, baseColorTransform1, {0, 1, 0, 0}) \
    X(vec4, normalTransform0, {1, 0, 0, 0}) \
    X(vec4, normalTransform1, {0, 1, 0, 0}) \
    X(vec4, metalRoughTransform0, {1, 0, 0, 0}) \
    X(vec4, metalRoughTransform1, {0, 1, 0, 0}) \
    X(vec4, occlusionTransform0, {1, 0, 0, 0}) \
    X(vec4, occlusionTransform1, {0, 1, 0, 0}) \
    X(vec4, emissiveTransform0, {1, 0, 0, 0}) \
    X(vec4, emissiveTransform1, {0, 1, 0, 0}) \
    /* clearcoat dual-layer material properties. */ \
    /* Ported from StandardMaterial clearCoat/clearCoatGloss/clearCoatBumpiness. */ \
    X(float, clearCoatFactor, 0.0f) \
    X(float, clearCoatRoughness, 0.0f) \
    X(float, clearCoatBumpiness, 1.0f) \
    X(float, heightMapFactor, 0.0f) \
    X(float, anisotropy, 0.0f) \
    X(float, transmissionFactor, 0.0f) \
    X(float, refractionIndex, 1.5f) \
    X(float, thickness, 0.0f) \
    /* --- Sheen (KHR_materials_sheen) --- */ \
    /* fabric/velvet sheen layer. */ \
    X(vec4, sheenColor, {0, 0, 0, 0}) \
    /* --- Iridescence (KHR_materials_iridescence) --- */ \
    /* thin-film interference layer. */ \
    X(vec4, iridescenceParams, {0, 1.3f, 100.0f, 400.0f}) \
    /* --- Spec-Gloss (KHR_materials_pbrSpecularGlossiness) --- */ \
    /* alternative PBR parameterization. */ \
    X(vec4, specGlossParams, {1, 1, 1, 1}) \
    /* --- Detail Normals + Displacement --- */ \
    /* detail normal overlay and vertex displacement. */ \
    X(vec4, detailDisplacementParams, {1, 0, 0.5f, 0}) \
    /* --- Detail Normal UV Transform --- */ \
    X(vec4, detailNormalTransform0, {1, 0, 0, 0}) \
    X(vec4, detailNormalTransform1, {0, 1, 0, 0}) \
    /* --- Volume Attenuation (KHR_materials_volume) + Dispersion (KHR_materials_dispersion) --- */ \
    X(vec4, attenuationParams, {1, 1, 1, 0}) \
    /* x=dispersion strength, y=alphaDither (<0 = unset, dither follows opacity), zw=pad */ \
    X(vec4, dispersionParams, {0, -1.0f, 0, 0}) \
    /* --- Scalar maps (upstream glossMap / thicknessMap / refractionMap) --- */ \
    /* x = the gloss factor that the gloss map modulates; y,z,w = which channel of */ \
    /* the gloss / thickness / refraction map to read (0=r,1=g,2=b,3=a). A NEGATIVE */ \
    /* channel means "no map bound": the flags word has no spare bits left (25-27 */ \
    /* and 29-31 carry the two dither modes), so presence rides in the sign here. */ \
    X(vec4, mapChannelParams, {1.0f, -1.0f, -1.0f, -1.0f}) \
    /* --- Parallax occlusion mapping --- */ \
    /* x = height-map base: the texel value that reads as the ORIGINAL surface, so */ \
    /* anything above it stands proud and anything below sinks in. 0 keeps the whole */ \
    /* map below the surface, which is what this port did before the field existed. */ \
    /* y = self-shadow strength (0 = off); the directional light marches the height */ \
    /* field and darkens texels its ray passes over. zw = pad. */ \
    X(vec4, heightMapParams, {0.0f, 0.0f, 0.0f, 0.0f})

namespace visutwin::canvas
{
    /// Shader-language declaration of the material block, emitted from the one
    /// field list. `msl` picks Metal spelling (float4), otherwise GLSL (vec4).
    inline std::string materialUniformDeclaration(const bool msl)
    {
        std::string s;
#define VT_EMIT_MATERIAL_FIELD(type, name, ...) \
        s += std::string("    ") + \
            (std::string(#type) == "vec4" ? (msl ? "float4" : "vec4") : #type) + \
            " " + #name + ";\n";
        VT_MATERIAL_UNIFORM_FIELDS(VT_EMIT_MATERIAL_FIELD)
#undef VT_EMIT_MATERIAL_FIELD
        return s;
    }
}
