// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanRenderPipeline.h"
#include "vulkanGraphicsDevice.h"
#include "vulkanShader.h"
#include "vulkanUtils.h"
#include "vulkan/vulkan_shader_bundle.h"

#include <stdexcept>

#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/stencilParameters.h"
#include "platform/graphics/shaderFeatures.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        void validateGeneratedForwardLayout()
        {
            using namespace vulkan_generated;
            static_assert(kForwardVertPushConstantSize == 128);
            static_assert(kForwardInstancedVertPushConstantSize == 128);
            static_assert(kForwardDynamicBatchVertPushConstantSize == 128);
            static_assert(kForwardSkinnedVertPushConstantSize == 128);
            static_assert(kForwardMorphedVertPushConstantSize == 128);
            static_assert(kForwardSkinnedMorphedVertPushConstantSize == 128);
            static_assert(kForwardSkyVertPushConstantSize == 128);
            static_assert(kForwardColorVertPushConstantSize == 128);
            static_assert(kForwardPointVertPushConstantSize == 128);
            static_assert(sizeof(VulkanLightingUBO) == 2000);
            static_assert(
                offsetof(MaterialUniforms, emissiveTransform1) +
                    sizeof(float) * 4 ==
                224);
            static_assert(sizeof(MaterialUniforms) == 384);

            for (const auto& reflected : kForwardFragBindings) {
                const bool uniform =
                    reflected.kind == ReflectedDescriptorKind::UniformBuffer;
                const bool sampler = reflected.kind ==
                    ReflectedDescriptorKind::CombinedImageSampler;
                const bool separateImage = reflected.kind ==
                    ReflectedDescriptorKind::SampledImage;
                const bool separateSampler = reflected.kind ==
                    ReflectedDescriptorKind::Sampler;
                const bool validMaterial =
                    reflected.set == 0 && reflected.binding == 0 && uniform &&
                    reflected.blockSize == sizeof(MaterialUniforms);
                const bool validLighting =
                    reflected.set == 2 && reflected.binding == 0 && uniform &&
                    reflected.blockSize == sizeof(VulkanLightingUBO);
                const bool validMaterialTexture =
                    reflected.set == 1 && reflected.binding < 26 &&
                    (sampler || separateImage || separateSampler);
                const bool validSceneTexture =
                    reflected.set == 3 && reflected.binding < 17 &&
                    (sampler || separateImage || separateSampler);
                const bool validGeometry =
                    reflected.set == 4 &&
                    ((reflected.binding < 2 &&
                      reflected.kind == ReflectedDescriptorKind::StorageBuffer) ||
                     (reflected.binding == 2 && uniform &&
                      reflected.blockSize == 80));
                const bool validCluster =
                    reflected.set == 5 && reflected.binding < 2 &&
                    reflected.kind == ReflectedDescriptorKind::StorageBuffer;
                if (!validMaterial && !validLighting &&
                    !validMaterialTexture && !validSceneTexture &&
                    !validGeometry && !validCluster) {
                    throw std::runtime_error(
                        "VulkanRenderPipeline: reflected shader layout is "
                        "incompatible with the engine descriptor contract");
                }
            }
        }

        int semanticLocation(const VertexSemantic semantic)
        {
            switch (semantic) {
                case VertexSemantic::SEMANTIC_POSITION: return 0;
                case VertexSemantic::SEMANTIC_NORMAL: return 1;
                case VertexSemantic::SEMANTIC_TEXCOORD:
                case VertexSemantic::SEMANTIC_TEXCOORD0: return 2;
                case VertexSemantic::SEMANTIC_TANGENT: return 3;
                case VertexSemantic::SEMANTIC_TEXCOORD1: return 4;
                case VertexSemantic::SEMANTIC_COLOR: return 5;
                case VertexSemantic::SEMANTIC_BLENDWEIGHT: return 11;
                case VertexSemantic::SEMANTIC_BLENDINDICES: return 12;
                default:
                    break;
            }

            const int semanticValue = static_cast<int>(semantic);
            const int attr0 = static_cast<int>(VertexSemantic::SEMANTIC_ATTR0);
            const int attr15 = static_cast<int>(VertexSemantic::SEMANTIC_ATTR15);
            return semanticValue >= attr0 && semanticValue <= attr15
                ? semanticValue - attr0 : -1;
        }

        VkFormat vertexElementFormat(const VertexElement& element)
        {
            const auto count = element.componentCount;
            switch (element.dataType) {
                case VertexDataType::TYPE_FLOAT32:
                    switch (count) {
                        case 1: return VK_FORMAT_R32_SFLOAT;
                        case 2: return VK_FORMAT_R32G32_SFLOAT;
                        case 3: return VK_FORMAT_R32G32B32_SFLOAT;
                        case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
                        default: return VK_FORMAT_UNDEFINED;
                    }
                case VertexDataType::TYPE_FLOAT16:
                    switch (count) {
                        case 1: return VK_FORMAT_R16_SFLOAT;
                        case 2: return VK_FORMAT_R16G16_SFLOAT;
                        case 3: return VK_FORMAT_R16G16B16_SFLOAT;
                        case 4: return VK_FORMAT_R16G16B16A16_SFLOAT;
                        default: return VK_FORMAT_UNDEFINED;
                    }
                case VertexDataType::TYPE_INT32:
                    switch (count) {
                        case 1: return VK_FORMAT_R32_SINT;
                        case 2: return VK_FORMAT_R32G32_SINT;
                        case 3: return VK_FORMAT_R32G32B32_SINT;
                        case 4: return VK_FORMAT_R32G32B32A32_SINT;
                        default: return VK_FORMAT_UNDEFINED;
                    }
                case VertexDataType::TYPE_UINT32:
                    switch (count) {
                        case 1: return VK_FORMAT_R32_UINT;
                        case 2: return VK_FORMAT_R32G32_UINT;
                        case 3: return VK_FORMAT_R32G32B32_UINT;
                        case 4: return VK_FORMAT_R32G32B32A32_UINT;
                        default: return VK_FORMAT_UNDEFINED;
                    }
                case VertexDataType::TYPE_INT16:
                    if (element.normalized) {
                        switch (count) {
                            case 1: return VK_FORMAT_R16_SNORM;
                            case 2: return VK_FORMAT_R16G16_SNORM;
                            case 3: return VK_FORMAT_R16G16B16_SNORM;
                            case 4: return VK_FORMAT_R16G16B16A16_SNORM;
                            default: return VK_FORMAT_UNDEFINED;
                        }
                    }
                    switch (count) {
                        case 1: return VK_FORMAT_R16_SINT;
                        case 2: return VK_FORMAT_R16G16_SINT;
                        case 3: return VK_FORMAT_R16G16B16_SINT;
                        case 4: return VK_FORMAT_R16G16B16A16_SINT;
                        default: return VK_FORMAT_UNDEFINED;
                    }
                case VertexDataType::TYPE_UINT16:
                    if (element.normalized) {
                        switch (count) {
                            case 1: return VK_FORMAT_R16_UNORM;
                            case 2: return VK_FORMAT_R16G16_UNORM;
                            case 3: return VK_FORMAT_R16G16B16_UNORM;
                            case 4: return VK_FORMAT_R16G16B16A16_UNORM;
                            default: return VK_FORMAT_UNDEFINED;
                        }
                    }
                    switch (count) {
                        case 1: return VK_FORMAT_R16_UINT;
                        case 2: return VK_FORMAT_R16G16_UINT;
                        case 3: return VK_FORMAT_R16G16B16_UINT;
                        case 4: return VK_FORMAT_R16G16B16A16_UINT;
                        default: return VK_FORMAT_UNDEFINED;
                    }
                case VertexDataType::TYPE_INT8:
                    if (element.normalized) {
                        switch (count) {
                            case 1: return VK_FORMAT_R8_SNORM;
                            case 2: return VK_FORMAT_R8G8_SNORM;
                            case 3: return VK_FORMAT_R8G8B8_SNORM;
                            case 4: return VK_FORMAT_R8G8B8A8_SNORM;
                            default: return VK_FORMAT_UNDEFINED;
                        }
                    }
                    switch (count) {
                        case 1: return VK_FORMAT_R8_SINT;
                        case 2: return VK_FORMAT_R8G8_SINT;
                        case 3: return VK_FORMAT_R8G8B8_SINT;
                        case 4: return VK_FORMAT_R8G8B8A8_SINT;
                        default: return VK_FORMAT_UNDEFINED;
                    }
                case VertexDataType::TYPE_UINT8:
                    if (element.normalized) {
                        switch (count) {
                            case 1: return VK_FORMAT_R8_UNORM;
                            case 2: return VK_FORMAT_R8G8_UNORM;
                            case 3: return VK_FORMAT_R8G8B8_UNORM;
                            case 4: return VK_FORMAT_R8G8B8A8_UNORM;
                            default: return VK_FORMAT_UNDEFINED;
                        }
                    }
                    switch (count) {
                        case 1: return VK_FORMAT_R8_UINT;
                        case 2: return VK_FORMAT_R8G8_UINT;
                        case 3: return VK_FORMAT_R8G8B8_UINT;
                        case 4: return VK_FORMAT_R8G8B8A8_UINT;
                        default: return VK_FORMAT_UNDEFINED;
                    }
            }
            return VK_FORMAT_UNDEFINED;
        }

        bool hasSemantic(const std::shared_ptr<VertexFormat>& format, VertexSemantic semantic)
        {
            if (!format) return false;
            for (const auto& element : format->elements()) {
                if (element.semantic == semantic) return true;
            }
            return false;
        }
    }

    VulkanRenderPipeline::VulkanRenderPipeline(VulkanGraphicsDevice* device)
        : _device(device)
    {
        try {
            validateGeneratedForwardLayout();
            createLayouts();
        } catch (...) {
            destroy();
            throw;
        }
    }

    VulkanRenderPipeline::~VulkanRenderPipeline()
    {
        destroy();
    }

    void VulkanRenderPipeline::destroy() noexcept
    {
        VkDevice vk = _device->device();
        for (auto& [key, pipeline] : _cache) {
            vkDestroyPipeline(vk, pipeline, nullptr);
        }
        _cache.clear();
        if (_pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(vk, _pipelineLayout, nullptr);
            _pipelineLayout = VK_NULL_HANDLE;
        }
        if (_materialSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, _materialSetLayout, nullptr);
            _materialSetLayout = VK_NULL_HANDLE;
        }
        if (_textureSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, _textureSetLayout, nullptr);
            _textureSetLayout = VK_NULL_HANDLE;
        }
        if (_lightingSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, _lightingSetLayout, nullptr);
            _lightingSetLayout = VK_NULL_HANDLE;
        }
        if (_sceneSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, _sceneSetLayout, nullptr);
            _sceneSetLayout = VK_NULL_HANDLE;
        }
        if (_geometrySetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, _geometrySetLayout, nullptr);
            _geometrySetLayout = VK_NULL_HANDLE;
        }
        if (_clusterSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, _clusterSetLayout, nullptr);
            _clusterSetLayout = VK_NULL_HANDLE;
        }
        if (_gpuDrivenSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(vk, _gpuDrivenSetLayout, nullptr);
            _gpuDrivenSetLayout = VK_NULL_HANDLE;
        }
    }

    void VulkanRenderPipeline::createLayouts()
    {
        VkDevice vk = _device->device();
        const auto createSetLayout =
            [vk](const VkDescriptorSetLayoutCreateInfo& info,
                 VkDescriptorSetLayout& layout, const char* name) {
                if (vkCreateDescriptorSetLayout(
                        vk, &info, nullptr, &layout) != VK_SUCCESS) {
                    throw std::runtime_error(
                        std::string(
                            "VulkanRenderPipeline: failed to create ") +
                        name + " descriptor set layout");
                }
            };

        // Set 0: per-draw Material UBO (dynamic — one descriptor set, the ring
        // buffer offset varies per draw).
        VkDescriptorSetLayoutBinding materialBinding{};
        materialBinding.binding = 0;
        materialBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        materialBinding.descriptorCount = 1;
        // Vertex too: displacement reads its scale/bias from this block.
        materialBinding.stageFlags =
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo materialLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        materialLayoutInfo.bindingCount = 1;
        materialLayoutInfo.pBindings = &materialBinding;
        createSetLayout(
            materialLayoutInfo, _materialSetLayout, "material");

        // Set 1: only statically-used material slots. Binding numbers remain
        // the engine texture-slot numbers, but unused gaps consume no sampler
        // descriptors (important on MoltenVK's 16-sampler stage limit).
        // 17 (height/parallax), 23 (detail normal) and 25 (displacement) are
        // separate images sharing the sampler at 24, so they cost no additional
        // per-stage sampler slots. 25 is sampled in the vertex stage.
        constexpr std::array<uint32_t, 10> textureSlots =
            {0, 1, 3, 4, 5, 17, 19, 23, 24, 25};
        std::array<VkDescriptorSetLayoutBinding, textureSlots.size()> texBindings{};
        for (uint32_t i = 0; i < texBindings.size(); i++) {
            const uint32_t binding = textureSlots[i];
            texBindings[i].binding = binding;
            texBindings[i].descriptorType =
                binding == 24 ? VK_DESCRIPTOR_TYPE_SAMPLER
                              : (binding == 17 || binding == 23 || binding == 25
                                     ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                     : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            texBindings[i].descriptorCount = 1;
            texBindings[i].stageFlags = binding == 25
                ? VK_SHADER_STAGE_VERTEX_BIT
                : (binding == 24
                       ? (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                       : VK_SHADER_STAGE_FRAGMENT_BIT);
        }

        VkDescriptorSetLayoutCreateInfo textureLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        textureLayoutInfo.bindingCount = static_cast<uint32_t>(texBindings.size());
        textureLayoutInfo.pBindings = texBindings.data();
        createSetLayout(
            textureLayoutInfo, _textureSetLayout, "texture");

        // Set 2: per-pass lighting/environment UBO (dynamic).
        VkDescriptorSetLayoutBinding lightingBinding{};
        lightingBinding.binding = 0;
        lightingBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        lightingBinding.descriptorCount = 1;
        lightingBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo lightingLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lightingLayoutInfo.bindingCount = 1;
        lightingLayoutInfo.pBindings = &lightingBinding;
        createSetLayout(
            lightingLayoutInfo, _lightingSetLayout, "lighting");

        // Set 3: per-pass scene textures.  Binding 0 = environment atlas
        // (equirectangular IBL + skybox source), binding 1 = directional
        // cascaded shadow-map depth atlas, bindings 2-3 = local spot-light 2D
        // depth maps, bindings 4-5 = omni point-light cubemap depth maps,
        // binding 6 = high-res skybox cubemap.  All are combined image
        // samplers; the sampler-cube vs sampler-2D distinction is a
        // shader-side concern, not a layout one.
        // Bindings 0-5 stay combined image samplers; 6-11 are separate images that
        // share the two sampler descriptors at 12-13. Combined bindings each cost
        // a per-stage sampler slot, and this device allows only 16 across all sets.
        // Bindings 14-16 (clustered shadow atlas, planar reflection colour and
        // depth) are separate images like 6-11; they sit after the samplers only
        // so those keep their existing bindings.
        std::array<VkDescriptorSetLayoutBinding, 17> sceneBindings{};
        for (uint32_t i = 0; i < sceneBindings.size(); ++i) {
            sceneBindings[i].binding = i;
            sceneBindings[i].descriptorType = vulkanSceneDescriptorType(i);
            sceneBindings[i].descriptorCount = 1;
            sceneBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo sceneLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sceneLayoutInfo.bindingCount = static_cast<uint32_t>(sceneBindings.size());
        sceneLayoutInfo.pBindings = sceneBindings.data();
        createSetLayout(
            sceneLayoutInfo, _sceneSetLayout, "scene");

        // Set 4: optional per-draw geometry resources. Palette matrices and
        // morph deltas are storage buffers; the compact 80-byte morph
        // parameter block is a regular UBO. Only bindings statically used by
        // the selected vertex variant need to be populated.
        std::array<VkDescriptorSetLayoutBinding, 3> geometryBindings{};
        geometryBindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        geometryBindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        geometryBindings[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo geometryLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        geometryLayoutInfo.bindingCount =
            static_cast<uint32_t>(geometryBindings.size());
        geometryLayoutInfo.pBindings = geometryBindings.data();
        createSetLayout(
            geometryLayoutInfo, _geometrySetLayout, "geometry");
        std::array<VkDescriptorSetLayoutBinding, 2> clusterBindings{};
        for (uint32_t i = 0; i < clusterBindings.size(); ++i) {
            clusterBindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo clusterLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        clusterLayoutInfo.bindingCount = 2;
        clusterLayoutInfo.pBindings = clusterBindings.data();
        createSetLayout(
            clusterLayoutInfo, _clusterSetLayout, "cluster");

        // Set 6: particle / Gaussian-splat storage and per-draw parameters.
        std::array<VkDescriptorSetLayoutBinding, 4> gpuBindings{};
        for (uint32_t i = 0; i < 3; ++i) {
            gpuBindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        }
        gpuBindings[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo gpuLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        gpuLayoutInfo.bindingCount = static_cast<uint32_t>(gpuBindings.size());
        gpuLayoutInfo.pBindings = gpuBindings.data();
        createSetLayout(
            gpuLayoutInfo, _gpuDrivenSetLayout, "GPU-driven");

        // Push constants: 2 × mat4 = 128 bytes
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 128;

        VkDescriptorSetLayout setLayouts[] = {
            _materialSetLayout, _textureSetLayout, _lightingSetLayout,
            _sceneSetLayout, _geometrySetLayout, _clusterSetLayout,
            _gpuDrivenSetLayout};

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 7;
        layoutInfo.pSetLayouts = setLayouts;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(
                vk, &layoutInfo, nullptr,
                &_pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanRenderPipeline: pipeline layout creation failed");
        }
    }

    VkPipeline VulkanRenderPipeline::get(const Primitive& primitive,
        const std::shared_ptr<VertexFormat>& vertexFormat,
        const std::shared_ptr<VertexFormat>& instanceFormat,
        const std::shared_ptr<VulkanShader>& shader,
        const std::shared_ptr<BlendState>& blendState,
        const std::shared_ptr<DepthState>& depthState,
        CullMode cullMode,
        bool stencilEnabled,
        const std::shared_ptr<StencilParameters>& stencilFront,
        const std::shared_ptr<StencilParameters>& stencilBack,
        const std::span<const VkFormat> colorFormats,
        VkFormat depthFormat,
        bool isSkybox)
    {
        // FNV-1a hash of pipeline state
        uint64_t hash = 14695981039346656037ULL;
        auto mix = [&](uint64_t v) { hash ^= v; hash *= 1099511628211ULL; };
        mix(static_cast<uint64_t>(primitive.type));
        mix(vertexFormat ? vertexFormat->renderingHash() : 0);
        mix(shader ? static_cast<uint64_t>(shader->id()) : 0);
        mix(blendState ? blendState->key() : 0);
        mix(depthState ? depthState->key() : 0);
        mix(static_cast<uint64_t>(cullMode));
        mix(stencilEnabled ? 1ull : 0ull);
        mix(stencilFront ? stencilFront->stateKey() : 0);
        mix(stencilBack ? stencilBack->stateKey() : 0);
        mix(static_cast<uint64_t>(colorFormats.size()));
        for (const VkFormat colorFormat : colorFormats) {
            mix(static_cast<uint64_t>(colorFormat));
        }
        mix(static_cast<uint64_t>(depthFormat));
        mix(instanceFormat ? instanceFormat->renderingHash() : 0);
        mix(isSkybox ? 1ull : 0ull);

        auto it = _cache.find(hash);
        if (it != _cache.end()) return it->second;

        VkPipeline pipeline = create(primitive, vertexFormat, instanceFormat, shader,
            blendState, depthState, cullMode, stencilEnabled, stencilFront, stencilBack,
            colorFormats, depthFormat, isSkybox);
        if (pipeline != VK_NULL_HANDLE) {
            _cache[hash] = pipeline;
        }
        return pipeline;
    }

    VkPipeline VulkanRenderPipeline::create(const Primitive& primitive,
        const std::shared_ptr<VertexFormat>& vertexFormat,
        const std::shared_ptr<VertexFormat>& instanceFormat,
        const std::shared_ptr<VulkanShader>& shader,
        const std::shared_ptr<BlendState>& blendState,
        const std::shared_ptr<DepthState>& depthState,
        CullMode cullMode,
        bool stencilEnabled,
        const std::shared_ptr<StencilParameters>& stencilFront,
        const std::shared_ptr<StencilParameters>& stencilBack,
        const std::span<const VkFormat> colorFormats,
        VkFormat depthFormat,
        bool isSkybox)
    {
        VkDevice vk = _device->device();

        // Use the instanced vertex stage only when the draw carries a
        // per-instance buffer AND the shader provides that variant.
        const bool instanced = instanceFormat &&
            shader->instancedVertexModule() != VK_NULL_HANDLE;
        const bool useSky = isSkybox && shader->skyVertexModule() != VK_NULL_HANDLE;
        const bool usePoint = !useSky && !instanced &&
            primitive.type == PRIMITIVE_POINTS &&
            hasSemantic(vertexFormat, VertexSemantic::SEMANTIC_COLOR) &&
            shader->pointVertexModule() != VK_NULL_HANDLE;
        const bool useColor = !useSky && !instanced && !usePoint &&
            hasSemantic(vertexFormat, VertexSemantic::SEMANTIC_COLOR) &&
            shader->colorVertexModule() != VK_NULL_HANDLE;
        const uint64_t features = shader->featureMask();
        const bool dynamicBatch =
            (features & shaderFeatureBit(ShaderFeature::DynamicBatch)) != 0;
        const bool skinned =
            (features & shaderFeatureBit(ShaderFeature::Skinning)) != 0;
        const bool morphed =
            (features & shaderFeatureBit(ShaderFeature::Morphing)) != 0;

        // --- Shader stages ---
        // Feature specialization constants. BOTH stages need them: the vertex shaders
        // gate displacement and the UV-space lightmap bake on the same mask, and giving
        // it only to the fragment stage silently disabled those.
        struct FeatureSpecialization {
            uint32_t low;
            uint32_t high;
        };
        static constexpr std::array<VkSpecializationMapEntry, 2>
            featureEntries = {{
                {0, offsetof(FeatureSpecialization, low), sizeof(uint32_t)},
                {1, offsetof(FeatureSpecialization, high), sizeof(uint32_t)},
            }};
        FeatureSpecialization featureData{};
        VkSpecializationInfo featureInfo{};
        if (shader->specializesFeatures()) {
            const uint64_t featureMask = shader->featureMask();
            featureData.low = static_cast<uint32_t>(featureMask);
            featureData.high = static_cast<uint32_t>(featureMask >> 32u);
            featureInfo.mapEntryCount = static_cast<uint32_t>(featureEntries.size());
            featureInfo.pMapEntries = featureEntries.data();
            featureInfo.dataSize = sizeof(featureData);
            featureInfo.pData = &featureData;
        }

        std::vector<VkPipelineShaderStageCreateInfo> stages;
        VkShaderModule vertModule = shader->vertexModule();
        if (useColor) vertModule = shader->colorVertexModule();
        if (usePoint) vertModule = shader->pointVertexModule();
        if (instanced) vertModule = shader->instancedVertexModule();
        if (useSky) vertModule = shader->skyVertexModule();
        if (morphed) vertModule = shader->morphedVertexModule();
        if (skinned) vertModule = shader->skinnedVertexModule();
        if (skinned && morphed)
            vertModule = shader->skinnedMorphedVertexModule();
        if (dynamicBatch)
            vertModule = shader->dynamicBatchVertexModule();
        if (vertModule != VK_NULL_HANDLE) {
            VkPipelineShaderStageCreateInfo vert{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            vert.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vert.module = vertModule;
            vert.pName = "main";
            if (shader->specializesFeatures()) {
                vert.pSpecializationInfo = &featureInfo;
            }
            stages.push_back(vert);
        }
        // Depth-only passes (shadow maps: no color formats) omit the
        // fragment stage entirely.  Depth is written from rasterization, and a
        // fragment shader that declares a colour output with no colour
        // attachment is a MoltenVK hazard that silently drops depth writes.
        const bool depthOnly = colorFormats.empty();
        if (!depthOnly && shader->fragmentModule() != VK_NULL_HANDLE) {
            VkPipelineShaderStageCreateInfo frag{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            frag.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            frag.module = shader->fragmentModule();
            frag.pName = "main";
            if (shader->specializesFeatures()) {
                frag.pSpecializationInfo = &featureInfo;
            }
            stages.push_back(frag);
        }

        // --- Vertex input ---
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        auto appendFormat = [&](const std::shared_ptr<VertexFormat>& format,
                                const uint32_t binding,
                                const VkVertexInputRate inputRate) {
            if (!format || format->size() <= 0 || format->elements().empty()) {
                spdlog::error(
                    "VulkanRenderPipeline: draw requires a vertex format with declared attributes");
                return false;
            }

            bindings.push_back(
                {binding, static_cast<uint32_t>(format->size()), inputRate});
            for (const auto& element : format->elements()) {
                const int location = semanticLocation(element.semantic);
                const VkFormat vkFormat = vertexElementFormat(element);
                if (location < 0 || vkFormat == VK_FORMAT_UNDEFINED) {
                    spdlog::error(
                        "VulkanRenderPipeline: unsupported vertex element semantic={} type={} components={}",
                        static_cast<int>(element.semantic),
                        static_cast<int>(element.dataType),
                        element.componentCount);
                    return false;
                }
                attributes.push_back({
                    static_cast<uint32_t>(location), binding, vkFormat, element.offset});
            }
            return true;
        };

        const bool gpuDrivenVertex =
            shader->name() == "particles" || shader->name() == "gsplat";
        if ((!gpuDrivenVertex &&
             !appendFormat(vertexFormat, 0, VK_VERTEX_INPUT_RATE_VERTEX)) ||
            (instanced && !appendFormat(instanceFormat, 1, VK_VERTEX_INPUT_RATE_INSTANCE))) {
            return VK_NULL_HANDLE;
        }

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        // --- Input assembly ---
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = vulkanMapPrimitiveType(primitive.type);
        inputAssembly.primitiveRestartEnable =
            primitive.type == PRIMITIVE_LINESTRIP ||
            primitive.type == PRIMITIVE_TRISTRIP;

        // --- Viewport (dynamic) ---
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        // --- Rasterization ---
        VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterization.depthClampEnable = VK_FALSE;
        rasterization.rasterizerDiscardEnable = VK_FALSE;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = vulkanMapCullMode(cullMode);
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        // Depth bias values come from dynamic state (vkCmdSetDepthBias) so
        // decals can toggle bias per draw without a pipeline permutation.
        // Bias of (0, 0, 0) — the device default — is a no-op.
        rasterization.depthBiasEnable = VK_TRUE;
        rasterization.lineWidth = 1.0f;

        // --- Multisample ---
        VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // --- Depth/stencil ---
        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        if (depthOnly) {
            // Shadow / depth-prepass: always test + write.  Vulkan gates depth
            // writes on depthTestEnable — if it is VK_FALSE, depthWriteEnable is
            // ignored and nothing is written, leaving the shadow atlas empty.
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
        } else if (depthState) {
            depthStencil.depthTestEnable = depthState->depthTest() ? VK_TRUE : VK_FALSE;
            depthStencil.depthWriteEnable = depthState->depthWrite() ? VK_TRUE : VK_FALSE;
        } else {
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
        }
        // Depth comparison follows the material's DepthState (upstream Material.depthFunc);
        // LessEqual is the default and the depth-only passes always use it.
        depthStencil.depthCompareOp = (!depthOnly && depthState)
            ? vulkanMapStencilCompare(depthState->func())
            : VK_COMPARE_OP_LESS_OR_EQUAL;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        const bool hasStencil = vulkanFormatHasStencil(depthFormat);
        const bool useStencil = stencilEnabled && hasStencil &&
            (stencilFront || stencilBack);
        depthStencil.stencilTestEnable = useStencil ? VK_TRUE : VK_FALSE;
        if (useStencil) {
            const auto& effectiveFront = stencilFront ? stencilFront : stencilBack;
            const auto& effectiveBack = stencilBack ? stencilBack : stencilFront;
            const auto makeStencilState = [](const StencilParameters& parameters) {
                VkStencilOpState state{};
                state.failOp = vulkanMapStencilOperation(parameters.failOperation());
                state.passOp = vulkanMapStencilOperation(parameters.passOperation());
                state.depthFailOp = vulkanMapStencilOperation(parameters.depthFailOperation());
                state.compareOp = vulkanMapStencilCompare(parameters.compareFunction());
                state.compareMask = parameters.readMask();
                state.writeMask = parameters.writeMask();
                // Reference is dynamic so changing it does not create a pipeline.
                state.reference = 0;
                return state;
            };
            depthStencil.front = makeStencilState(*effectiveFront);
            depthStencil.back = makeStencilState(*effectiveBack);
        }

        // --- Color blend ---
        // An empty color-format list means depth-only (e.g. shadow map pass).
        // The pipeline must have zero colour attachments to match the render
        // pass attached at draw time (VUID-vkCmdDrawIndexed-colorAttachmentCount-06179).
        VkPipelineColorBlendAttachmentState blendAttachment{};
        if (blendState && blendState->enabled()) {
            // VK_BLEND_FACTOR_SRC1_* is only legal when the dualSrcBlend feature was
            // enabled at device creation; a pipeline using it otherwise is invalid.
            // Callers are expected to gate on supportsDualSourceBlending(), so reaching
            // here means a caller bug — say so loudly and degrade to the single-source
            // equivalent rather than handing the driver an invalid pipeline.
            const bool dualSrcOk = !blendState->usesDualSourceBlending() ||
                _device->supportsDualSourceBlending();
            if (!dualSrcOk) {
                spdlog::error("Vulkan: blend state uses dual-source factors but the device "
                    "has no dualSrcBlend support — degrading to single-source blending. "
                    "Check GraphicsDevice::supportsDualSourceBlending() before building "
                    "a BLENDMODE_SRC1_* blend state.");
            }
            const auto mapFactor = [dualSrcOk](const int factor) {
                return vulkanMapBlendFactor(dualSrcOk ? factor : blendFactorWithoutSrc1(factor));
            };

            blendAttachment.blendEnable = VK_TRUE;
            blendAttachment.srcColorBlendFactor = mapFactor(blendState->colorSrcFactor());
            blendAttachment.dstColorBlendFactor = mapFactor(blendState->colorDstFactor());
            blendAttachment.colorBlendOp = vulkanMapBlendOp(blendState->colorOp());
            blendAttachment.srcAlphaBlendFactor = mapFactor(blendState->alphaSrcFactor());
            blendAttachment.dstAlphaBlendFactor = mapFactor(blendState->alphaDstFactor());
            blendAttachment.alphaBlendOp = vulkanMapBlendOp(blendState->alphaOp());
        }
        if (!blendState || blendState->redWrite()) {
            blendAttachment.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
        }
        if (!blendState || blendState->greenWrite()) {
            blendAttachment.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
        }
        if (!blendState || blendState->blueWrite()) {
            blendAttachment.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
        }
        if (!blendState || blendState->alphaWrite()) {
            blendAttachment.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
        }

        // Dynamic rendering requires the pipeline to declare the same ordered
        // color attachment list as VkRenderingInfo. The shader may write only
        // a subset of locations; unwritten attachments retain their load value.
        std::vector<VkPipelineColorBlendAttachmentState> blendAttachments(
            colorFormats.size(), blendAttachment);
        VkPipelineColorBlendStateCreateInfo colorBlend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlend.attachmentCount =
            static_cast<uint32_t>(blendAttachments.size());
        colorBlend.pAttachments =
            blendAttachments.empty() ? nullptr : blendAttachments.data();

        // --- Dynamic state ---
        VkDynamicState dynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        };
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = 4;
        dynamicState.pDynamicStates = dynamicStates;

        // --- Dynamic rendering (Vulkan 1.3) ---
        VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount =
            static_cast<uint32_t>(colorFormats.size());
        renderingInfo.pColorAttachmentFormats =
            colorFormats.empty() ? nullptr : colorFormats.data();
        renderingInfo.depthAttachmentFormat = depthFormat;
        renderingInfo.stencilAttachmentFormat = hasStencil ? depthFormat : VK_FORMAT_UNDEFINED;

        // --- Create pipeline ---
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = _pipelineLayout;
        pipelineInfo.renderPass = VK_NULL_HANDLE; // dynamic rendering

        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult result = vkCreateGraphicsPipelines(vk, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS) {
            spdlog::error("Failed to create Vulkan graphics pipeline: {}", static_cast<int>(result));
        }
        return pipeline;
    }
}

#endif // VISUTWIN_HAS_VULKAN
