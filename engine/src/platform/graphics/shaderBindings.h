// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// THE texture binding table: every material and scene texture slot of both backends,
// declared once. Everything else is derived from these two X-macro lists:
//   - Metal: MetalTextureBinder::kMaxTextureSlots (a material's maps are not rows but lanes
//     of the bindless table, bindlessTable.h; the rows are the quad inputs);
//   - Vulkan: kMaterialTextureBindings (set 1, in QUAD SLOT ORDER), the separate-image
//     predicate, kSceneTextureBindingCount and vulkanSceneDescriptorType (set 3);
//   - Vulkan's validateForwardLayout, which holds the forward and shadow programs'
//     reflected bindings to the set-1 and set-3 rows;
//   - the generated Slang declarations (tools/generate_shader_bindings.py ->
//     bindings.slang), where a declaration carries BOTH `register(tN)`, the Metal slot,
//     and `[[vk::binding(b, set)]]`, the Vulkan one.
// A new slot is one row here. Two rules the rows encode: a material row is APPENDED,
// never inserted, because a Vulkan quad pass's input i is the i-th material row; and a
// Vulkan material texture is a SEPARATE image (SeparateImage, read through the shared
// sampler at the Sampler row) unless it is one of the six quad inputs that keep their own
// sampler, since MoltenVK's fragment stage is at its 16-sampler limit.
//
// Columns: X(name, type, kind, metalSlot, vkBinding, ownSampler, metalMaterialSlot)
//   type:     Tex2D | TexCube | Tex2DArray | Sampler
//   kind:     CombinedSampler (Vulkan: texture + its own sampler; Metal: a texture slot
//             and, with ownSampler, a sampler slot), SeparateImage (Vulkan: image through
//             the shared sampler), Sampler (a sampler state), MetalOnly (no Vulkan binding)
//   metalSlot: the Metal fragment texture slot; >= 100 is a VERTEX texture (slot - 100,
//             a vertex-stage slot would be 100 + its index; none is left);
//             for a Sampler row the Metal sampler slot; -1 for none (a constexpr sampler)
//   vkBinding: the Vulkan binding in the list's set (1 material, 3 scene); -1 for none
//   ownSampler: the map is read through its texture's own sampler state on both backends
//   metalMaterialSlot: a fragment slot the Metal material binder owns (clears when the
//             material has no map there); 0 for a slot another binder owns (quad input 2
//             is the scene env atlas on Metal) and for vertex and sampler rows
//
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace visutwin::canvas
{
    // Set 1 on Vulkan. Quad slot i is row i. Metal slots 0-5 are also the quad inputs 0-5.
#define VT_MATERIAL_TEXTURE_BINDINGS(X) \
    X(baseColorMap,       Tex2D,   CombinedSampler, 0,   0,  1, 0) \
    X(normalMap,          Tex2D,   CombinedSampler, 1,   1,  1, 0) \
    X(quadInput2,         Tex2D,   CombinedSampler, 2,   2,  0, 0) \
    X(metalRoughMap,      Tex2D,   CombinedSampler, 3,   3,  1, 0) \
    X(occlusionMap,       Tex2D,   CombinedSampler, 4,   4,  1, 0) \
    X(emissiveMap,        Tex2D,   CombinedSampler, 5,   5,  1, 0) \
    X(heightMap,          Tex2D,   SeparateImage,   17,  17, 0, 0) \
    X(lightMap,           Tex2D,   CombinedSampler, 19,  19, 1, 0) \
    X(detailNormalMap,    Tex2D,   SeparateImage,   23,  23, 0, 0) \
    X(materialSampler,    Sampler, Sampler,         0,   24, 0, 0) \
    X(clearCoatMap,       Tex2D,   SeparateImage,   7,   7,  0, 0) \
    X(clearCoatGlossMap,  Tex2D,   SeparateImage,   13,  13, 0, 0) \
    X(clearCoatNormalMap, Tex2D,   SeparateImage,   14,  14, 0, 0)

    // Set 3 on Vulkan: the per-pass scene textures. The VSM moment maps are Metal-only
    // slots: the spot pair because Vulkan rebinds the spot slots 2 / 3 with the linear
    // sampler, the directional pair because one specialised Metal library cannot give slot
    // 6 / 35 two types (Vulkan reads moments and depth from bindings 1 / 22). The user map is
    // the application's texture for its own chunk overrides (Scene::setUserShaderData), read
    // through the material sampler, which repeats.
#define VT_SCENE_TEXTURE_BINDINGS(X) \
    X(envAtlas,              Tex2D,      CombinedSampler, 2,  0,  0, 0) \
    X(shadowMap0,            Tex2D,      SeparateImage,   6,  1,  0, 0) \
    X(localShadowMap0,       Tex2D,      CombinedSampler, 11, 2,  0, 0) \
    X(localShadowMap1,       Tex2D,      CombinedSampler, 12, 3,  0, 0) \
    X(omniShadowCube0,       TexCube,    CombinedSampler, 15, 4,  0, 0) \
    X(omniShadowCube1,       TexCube,    CombinedSampler, 16, 5,  0, 0) \
    X(skyboxCube,            TexCube,    SeparateImage,   8,  6,  0, 0) \
    X(reflectionProbeCube,   TexCube,    SeparateImage,   24, 7,  0, 0) \
    X(areaLightLut1,         Tex2D,      SeparateImage,   20, 8,  0, 0) \
    X(areaLightLut2,         Tex2D,      SeparateImage,   21, 9,  0, 0) \
    X(sceneColorGrab,        Tex2D,      SeparateImage,   22, 10, 0, 0) \
    X(sceneDepthGrab,        Tex2D,      SeparateImage,   25, 11, 0, 0) \
    X(linearClampSampler,    Sampler,    Sampler,         -1, 12, 0, 0) \
    X(nearestClampSampler,   Sampler,    Sampler,         -1, 13, 0, 0) \
    X(clusterShadowAtlas,    Tex2D,      SeparateImage,   26, 14, 0, 0) \
    X(planarReflection,      Tex2D,      SeparateImage,   9,  15, 0, 0) \
    X(planarReflectionDepth, Tex2D,      SeparateImage,   10, 16, 0, 0) \
    X(cookie2D0,             Tex2D,      SeparateImage,   27, 17, 0, 0) \
    X(cookie2D1,             Tex2D,      SeparateImage,   28, 18, 0, 0) \
    X(cookieCube0,           TexCube,    SeparateImage,   29, 19, 0, 0) \
    X(cookieCube1,           TexCube,    SeparateImage,   30, 20, 0, 0) \
    X(ssao,                  Tex2D,      SeparateImage,   18, 21, 0, 0) \
    X(shadowMap1,            Tex2D,      SeparateImage,   35, 22, 0, 0) \
    X(clusterCookieAtlas,    Tex2D,      SeparateImage,   36, 23, 0, 0) \
    X(localVsmMap0,          Tex2D,      MetalOnly,       37, -1, 0, 0) \
    X(localVsmMap1,          Tex2D,      MetalOnly,       38, -1, 0, 0) \
    X(shadowMoments0,        Tex2D,      MetalOnly,       39, -1, 0, 0) \
    X(shadowMoments1,        Tex2D,      MetalOnly,       40, -1, 0, 0) \
    X(userMap,               Tex2D,      SeparateImage,   46, 24, 0, 0)

    // Quad inputs 8 and up: fragment-only SEPARATE images, named by their material row. On Metal
    // input n is texture n, on Vulkan the named row's binding; both read them through the quad
    // sampler (VT_DECLARE_QUAD_SAMPLER). Rows only a material draw uses, so a quad pass that
    // binds them takes nothing from a material.
#define VT_QUAD_EXTRA_INPUTS(X) X(detailNormalMap) X(clearCoatMap) X(clearCoatGlossMap) X(clearCoatNormalMap)

    enum class ShaderBindingType : uint8_t { Tex2D, TexCube, Tex2DArray, Sampler };
    enum class ShaderBindingKind : uint8_t { CombinedSampler, SeparateImage, Sampler, MetalOnly };

    struct ShaderBindingRow
    {
        const char* name;
        ShaderBindingType type;
        ShaderBindingKind kind;
        int metalSlot;
        int vkBinding;
        bool ownSampler;
        bool metalMaterialSlot;
    };

#define VT_SHADER_BINDING_ROW(name, type, kind, metalSlot, vkBinding, ownSampler, metalMaterial) \
    ShaderBindingRow{#name, ShaderBindingType::type, ShaderBindingKind::kind, metalSlot, vkBinding, \
        ownSampler != 0, metalMaterial != 0},

    inline constexpr std::array kMaterialBindingRows = {VT_MATERIAL_TEXTURE_BINDINGS(VT_SHADER_BINDING_ROW)};
    inline constexpr std::array kSceneBindingRows = {VT_SCENE_TEXTURE_BINDINGS(VT_SHADER_BINDING_ROW)};

#undef VT_SHADER_BINDING_ROW

    inline constexpr uint32_t kMaterialBindingSet = 1;
    inline constexpr uint32_t kSceneBindingSet = 3;

    namespace shaderBindings
    {
        /// Rows of `rows` satisfying `pred`, counted.
        template <size_t N, typename Pred>
        constexpr size_t countIf(const std::array<ShaderBindingRow, N>& rows, Pred pred)
        {
            size_t n = 0;
            for (const auto& row : rows) {
                if (pred(row)) {
                    ++n;
                }
            }
            return n;
        }

        /// The `metalSlot` of each row satisfying `pred`, in table order.
        template <size_t Count, size_t N, typename Pred>
        constexpr std::array<int, Count> metalSlotsIf(const std::array<ShaderBindingRow, N>& rows, Pred pred)
        {
            std::array<int, Count> out{};
            size_t i = 0;
            for (const auto& row : rows) {
                if (pred(row)) {
                    out[i++] = row.metalSlot;
                }
            }
            return out;
        }

        /// The `vkBinding` of each row satisfying `pred`, in table order.
        template <size_t Count, size_t N, typename Pred>
        constexpr std::array<uint32_t, Count> vkBindingsIf(const std::array<ShaderBindingRow, N>& rows, Pred pred)
        {
            std::array<uint32_t, Count> out{};
            size_t i = 0;
            for (const auto& row : rows) {
                if (pred(row)) {
                    out[i++] = static_cast<uint32_t>(row.vkBinding);
                }
            }
            return out;
        }

        constexpr bool isFragmentTextureSlot(const ShaderBindingRow& row)
        {
            return row.type != ShaderBindingType::Sampler && row.metalSlot >= 0 && row.metalSlot < 100;
        }

        /// The highest Metal fragment texture slot either table uses.
        constexpr int maxMetalTextureSlot()
        {
            int max = -1;
            for (const auto& row : kMaterialBindingRows) {
                if (isFragmentTextureSlot(row) && row.metalSlot > max) max = row.metalSlot;
            }
            for (const auto& row : kSceneBindingRows) {
                if (isFragmentTextureSlot(row) && row.metalSlot > max) max = row.metalSlot;
            }
            return max;
        }

        /// The highest Vulkan binding of the scene set.
        constexpr int maxSceneVkBinding()
        {
            int max = -1;
            for (const auto& row : kSceneBindingRows) {
                if (row.vkBinding > max) max = row.vkBinding;
            }
            return max;
        }

        /// The kind of a Vulkan material (set 1) binding; MetalOnly when the set has none.
        constexpr ShaderBindingKind materialBindingKind(const uint32_t binding)
        {
            for (const auto& row : kMaterialBindingRows) {
                if (row.vkBinding == static_cast<int>(binding)) return row.kind;
            }
            return ShaderBindingKind::MetalOnly;
        }

        /// The kind of a Vulkan scene (set 3) binding; MetalOnly when the set has none.
        constexpr ShaderBindingKind sceneBindingKind(const uint32_t binding)
        {
            for (const auto& row : kSceneBindingRows) {
                if (row.vkBinding == static_cast<int>(binding)) return row.kind;
            }
            return ShaderBindingKind::MetalOnly;
        }

        constexpr bool hasVkBinding(const ShaderBindingRow& row) { return row.vkBinding >= 0; }
        constexpr bool ownsSampler(const ShaderBindingRow& row) { return row.ownSampler; }
        constexpr bool isMetalMaterialSlot(const ShaderBindingRow& row) { return row.metalMaterialSlot; }
        constexpr bool isMaterialSamplerRow(const ShaderBindingRow& row)
        {
            return row.kind == ShaderBindingKind::Sampler;
        }
    }

    // ---- Derived constants, with the literals they replaced pinned so the tables above
    // ---- cannot drift from what the shaders of both backends declare.

    /// Vulkan set 1 bindings in quad-slot order (every material row).
    inline constexpr auto kMaterialTextureBindings =
        shaderBindings::vkBindingsIf<kMaterialBindingRows.size()>(kMaterialBindingRows, shaderBindings::hasVkBinding);
    static_assert(kMaterialTextureBindings ==
        std::array<uint32_t, 13>{0, 1, 2, 3, 4, 5, 17, 19, 23, 24, 7, 13, 14});

    /// The shared sampler every separate material image reads through (set 1).
    inline constexpr uint32_t kMaterialExtraSamplerBinding =
        shaderBindings::vkBindingsIf<1>(kMaterialBindingRows, shaderBindings::isMaterialSamplerRow)[0];
    static_assert(kMaterialExtraSamplerBinding == 24);

    /// Set 3 binding count: the highest scene binding plus one.
    inline constexpr uint32_t kSceneTextureBindingCount = static_cast<uint32_t>(shaderBindings::maxSceneVkBinding() + 1);
    static_assert(kSceneTextureBindingCount == 25);

    /// True for a set-1 binding declared as a SEPARATE image (`texture2D`) that the
    /// shader combines with the sampler at kMaterialExtraSamplerBinding.
    constexpr bool vulkanMaterialBindingIsSeparateImage(const uint32_t binding)
    {
        return shaderBindings::materialBindingKind(binding) == ShaderBindingKind::SeparateImage;
    }
    static_assert(vulkanMaterialBindingIsSeparateImage(7) && vulkanMaterialBindingIsSeparateImage(13) &&
        vulkanMaterialBindingIsSeparateImage(14) && vulkanMaterialBindingIsSeparateImage(17) &&
        vulkanMaterialBindingIsSeparateImage(23) &&
        !vulkanMaterialBindingIsSeparateImage(0) && !vulkanMaterialBindingIsSeparateImage(19) &&
        !vulkanMaterialBindingIsSeparateImage(24));

    namespace shaderBindings
    {
        constexpr bool sameName(const char* a, const char* b)
        {
            while (*a != '\0' && *a == *b) {
                ++a;
                ++b;
            }
            return *a == *b;
        }

        /// The Metal slot of the row called `name`; -1 when there is none.
        template <size_t N>
        constexpr int metalSlotOf(const std::array<ShaderBindingRow, N>& rows, const char* name)
        {
            for (const auto& row : rows) {
                if (sameName(row.name, name)) {
                    return row.metalSlot;
                }
            }
            return -1;
        }
    }

    /// The quad pass inputs: 0-7 are the first eight material rows, the rest the rows
    /// VT_QUAD_EXTRA_INPUTS names.
    inline constexpr size_t kQuadInputsFromRows = 8;
#define VT_QUAD_EXTRA_INPUT_NAME(name) #name,
    inline constexpr std::array kQuadExtraInputRows = {VT_QUAD_EXTRA_INPUTS(VT_QUAD_EXTRA_INPUT_NAME)};
#undef VT_QUAD_EXTRA_INPUT_NAME
    inline constexpr size_t kQuadTextureInputs = kQuadInputsFromRows + kQuadExtraInputRows.size();
    static_assert(kQuadTextureInputs == 12);

    namespace shaderBindings
    {
        /// The index in kMaterialTextureBindings (a Vulkan set-1 descriptor) of quad input `input`.
        constexpr size_t quadInputDescriptorIndex(const size_t input)
        {
            if (input < kQuadInputsFromRows) {
                return input;
            }
            size_t index = 0;
            for (const auto& row : kMaterialBindingRows) {
                if (!hasVkBinding(row)) {
                    continue;
                }
                if (sameName(row.name, kQuadExtraInputRows[input - kQuadInputsFromRows])) {
                    return index;
                }
                ++index;
            }
            return kMaterialTextureBindings.size();
        }

        constexpr bool quadExtraInputsAreSeparateImages()
        {
            for (size_t input = kQuadInputsFromRows; input < kQuadTextureInputs; ++input) {
                const size_t index = quadInputDescriptorIndex(input);
                if (index >= kMaterialTextureBindings.size() ||
                    !vulkanMaterialBindingIsSeparateImage(kMaterialTextureBindings[index])) {
                    return false;
                }
            }
            return true;
        }
    }
    static_assert(shaderBindings::quadExtraInputsAreSeparateImages(),
        "a quad input past 7 must be a separate image of the material set");
    static_assert(kMaterialTextureBindings[shaderBindings::quadInputDescriptorIndex(8)] == 23 &&
        kMaterialTextureBindings[shaderBindings::quadInputDescriptorIndex(11)] == 14);

    /// Metal fragment texture slots: 0 .. kMetalMaxTextureSlots - 1.
    inline constexpr int kMetalMaxTextureSlots = shaderBindings::maxMetalTextureSlot() + 1;
    static_assert(kMetalMaxTextureSlots == 47);

}
