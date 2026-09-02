// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Post-processing core for the Vulkan backend: compose (tonemap/bloom/
// vignette/CAS/DOF), SSAO, depth-aware blur, and TAA resolve — each a
// fullscreen draw recorded into the ALREADY ACTIVE render pass (the scene-side
// RenderPass* objects begin/end the pass and call the device execute hooks,
// same contract as the Metal backend and the VSM blur).
//
// Shaders are compiled at runtime from engine/shaders/vulkan via shaderc.
// Without shaderc (or on compile errors) the passes no-op, which matches the
// pre-port behavior of the base-class hooks.

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanGraphicsDevice.h"

#include <cstring>

#include "vulkanRenderTarget.h"
#include "vulkanTexture.h"
#include "vulkanUniformRingBuffer.h"
#include "vulkanUtils.h"
#include "vulkan/vulkan_shader_bundle.h"
#include "platform/graphics/texture.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{

    bool VulkanGraphicsDevice::ensurePostResources()
    {
        if (_postPipelineLayout != VK_NULL_HANDLE) {
            return _postVertModule != VK_NULL_HANDLE;
        }
        if (_postResourcesAttempted) {
            return false;
        }
        _postResourcesAttempted = true;

        // Bindings 0-3: pass inputs (scene/bloom/ssao/depth etc. per pass).
        std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
        for (uint32_t i = 0; i < 4; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        // Binding 4: params UBO (plain, transient — exact offset/range written
        // per execute; the data lives in the per-frame uniform ring).
        bindings[4].binding = 4;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[4].descriptorCount = 1;
        bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        for (uint32_t i = 5; i < 7; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        VkResult result =
            vkCreateDescriptorSetLayout(_device, &setInfo, nullptr, &_postSetLayout);
        if (result != VK_SUCCESS) {
            spdlog::error(
                "VulkanGraphicsDevice: post descriptor set layout creation failed ({})",
                static_cast<int>(result));
            return false;
        }

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &_postSetLayout;
        result = vkCreatePipelineLayout(
            _device, &layoutInfo, nullptr, &_postPipelineLayout);
        if (result != VK_SUCCESS) {
            spdlog::error(
                "VulkanGraphicsDevice: post pipeline layout creation failed ({})",
                static_cast<int>(result));
            vkDestroyDescriptorSetLayout(_device, _postSetLayout, nullptr);
            _postSetLayout = VK_NULL_HANDLE;
            return false;
        }

        VkShaderModuleCreateInfo vertexInfo{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        vertexInfo.codeSize =
            sizeof(vulkan_generated::kPostFullscreenVert);
        vertexInfo.pCode = vulkan_generated::kPostFullscreenVert;
        result = vkCreateShaderModule(
            _device, &vertexInfo, nullptr, &_postVertModule);
        if (result != VK_SUCCESS) {
            spdlog::error(
                "VulkanGraphicsDevice: post vertex shader module creation failed ({})",
                static_cast<int>(result));
            vkDestroyPipelineLayout(_device, _postPipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(_device, _postSetLayout, nullptr);
            _postPipelineLayout = VK_NULL_HANDLE;
            _postSetLayout = VK_NULL_HANDLE;
            return false;
        }

        // Linear clamp-to-edge sampler for all post inputs (kernel taps at the
        // frame border must not wrap — mirrors the Metal _postSampler).
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        result = vkCreateSampler(
            _device, &samplerInfo, nullptr, &_postSampler);
        if (result != VK_SUCCESS) {
            spdlog::error(
                "VulkanGraphicsDevice: post sampler creation failed ({})",
                static_cast<int>(result));
            vkDestroyShaderModule(_device, _postVertModule, nullptr);
            vkDestroyPipelineLayout(_device, _postPipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(_device, _postSetLayout, nullptr);
            _postVertModule = VK_NULL_HANDLE;
            _postPipelineLayout = VK_NULL_HANDLE;
            _postSetLayout = VK_NULL_HANDLE;
            return false;
        }

        return true;
    }

    VkShaderModule VulkanGraphicsDevice::postFragmentModule(const PostPassKind kind)
    {
        const auto index = static_cast<size_t>(kind);
        if (_postFragModules[index] != VK_NULL_HANDLE || _postFragCompileAttempted[index]) {
            return _postFragModules[index];
        }
        _postFragCompileAttempted[index] = true;

        const uint32_t* words = nullptr;
        size_t wordCount = 0;
        switch (kind) {
        case PostPassKind::Compose:
            words = vulkan_generated::kPostComposeFrag;
            wordCount = vulkan_generated::kPostComposeFragWordCount;
            break;
        case PostPassKind::Ssao:
            words = vulkan_generated::kPostSsaoFrag;
            wordCount = vulkan_generated::kPostSsaoFragWordCount;
            break;
        case PostPassKind::Taa:
            words = vulkan_generated::kPostTaaFrag;
            wordCount = vulkan_generated::kPostTaaFragWordCount;
            break;
        default:                      return VK_NULL_HANDLE;
        }

        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = wordCount * sizeof(uint32_t);
        info.pCode = words;
        const VkResult result = vkCreateShaderModule(
            _device, &info, nullptr, &_postFragModules[index]);
        if (result != VK_SUCCESS) {
            _postFragModules[index] = VK_NULL_HANDLE;
            spdlog::error(
                "VulkanGraphicsDevice: post fragment shader module creation failed ({})",
                static_cast<int>(result));
        }
        return _postFragModules[index];
    }

    VkPipeline VulkanGraphicsDevice::getPostPipeline(const PostPassKind kind,
        const VkFormat colorFormat, const VkFormat depthFormat)
    {
        const uint64_t key = (static_cast<uint64_t>(kind) << 56) |
                             (static_cast<uint64_t>(colorFormat) << 28) |
                             static_cast<uint64_t>(depthFormat);
        if (const auto it = _postPipelines.find(key); it != _postPipelines.end()) {
            return it->second;
        }

        VkShaderModule frag = postFragmentModule(kind);
        if (frag == VK_NULL_HANDLE || _postVertModule == VK_NULL_HANDLE) {
            _postPipelines[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = _postVertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;

        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &colorFormat;
        renderingInfo.depthAttachmentFormat = depthFormat;

        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = _postPipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
            spdlog::error("Vulkan post pipeline creation failed (pass {})", static_cast<int>(kind));
        }
        _postPipelines[key] = pipeline;
        return pipeline;
    }

    void VulkanGraphicsDevice::executePostPass(const PostPassKind kind,
        Texture* const textures[6], const void* paramsData, const size_t paramsSize)
    {
        if (!_frameActive || !_dynamicRenderingActive || !_uniformRing) {
            return;
        }
        if (!ensurePostResources()) {
            return;
        }

        // Formats of the active pass (same derivation as draw()).
        VkFormat colorFmt = _swapchainFormat;
        VkFormat depthFmt = _depthFormat;
        if (_activeOffscreenTarget) {
            const auto& colors = _activeOffscreenTarget->colorAttachments();
            colorFmt = colors.empty() ? VK_FORMAT_UNDEFINED : colors[0].format;
            depthFmt = _activeOffscreenTarget->hasDepthAttachment()
                ? _activeOffscreenTarget->depthAttachment().format
                : VK_FORMAT_UNDEFINED;
        }
        if (colorFmt == VK_FORMAT_UNDEFINED) {
            return;
        }

        VkPipeline pipeline = getPostPipeline(kind, colorFmt, depthFmt);
        if (pipeline == VK_NULL_HANDLE) {
            return;
        }

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        // Reserve uniform space before consuming a descriptor set. A failed
        // ring allocation is an explicit skipped pass, never an offset alias.
        const auto paramsOffset = allocateUniform(paramsData, paramsSize);
        if (!paramsOffset) {
            return;
        }

        const VkDescriptorSet set =
            allocateFrameDescriptorSet(_postSetLayout);
        if (set == VK_NULL_HANDLE) {
            return;
        }

        // Texture bindings: white fallback for absent slots so every declared
        // sampler is valid (statically-used descriptors must be bound).
        constexpr std::array<uint32_t, 6> textureBindings = {0, 1, 2, 3, 5, 6};
        std::array<VkDescriptorImageInfo, 6> imageInfos{};
        std::array<VkWriteDescriptorSet, 7> writes{};
        for (uint32_t i = 0; i < textureBindings.size(); ++i) {
            VkImageView view = _whiteImageView;
            VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (textures[i]) {
                if (auto* vkTex = static_cast<gpu::VulkanTexture*>(textures[i]->impl());
                    vkTex && vkTex->imageView() != VK_NULL_HANDLE) {
                    view = vkTex->imageView();
                    if (vkTex->isDepth()) {
                        // Depth reaches a post pass in one of two read-only layouts:
                        // endRenderPass leaves a texture-backed depth attachment in
                        // SHADER_READ_ONLY_OPTIMAL, while grabSceneDepth leaves its copy
                        // in DEPTH_STENCIL_READ_ONLY_OPTIMAL. Hard-coding the latter made
                        // every post draw that samples depth a layout mismatch. Use the
                        // layout the texture is actually tracked in.
                        const VkImageLayout tracked = vkTex->layout(0, 0);
                        imageLayout = tracked == VK_IMAGE_LAYOUT_UNDEFINED
                            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                            : tracked;
                    }
                }
            }
            imageInfos[i].sampler = _postSampler;
            imageInfos[i].imageView = view;
            imageInfos[i].imageLayout = imageLayout;

            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = textureBindings[i];
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo = &imageInfos[i];
        }

        // Params UBO: sub-allocate from the per-frame uniform ring and bind
        // the exact offset/range (the set is transient, no dynamic offsets).
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = _uniformRing->buffer();
        bufferInfo.offset = *paramsOffset;
        bufferInfo.range = paramsSize;
        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = set;
        writes[6].dstBinding = 4;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[6].descriptorCount = 1;
        writes[6].pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            _postPipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        // Force the material pipeline + descriptors to rebind on the next draw.
        _currentPipeline = VK_NULL_HANDLE;
    }

    void VulkanGraphicsDevice::destroyPostResources()
    {
        for (auto& [key, pipeline] : _postPipelines) {
            if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(_device, pipeline, nullptr);
        }
        _postPipelines.clear();
        for (auto& module : _postFragModules) {
            if (module != VK_NULL_HANDLE) vkDestroyShaderModule(_device, module, nullptr);
            module = VK_NULL_HANDLE;
        }
        if (_postVertModule != VK_NULL_HANDLE) {
            vkDestroyShaderModule(_device, _postVertModule, nullptr);
            _postVertModule = VK_NULL_HANDLE;
        }
        if (_postPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(_device, _postPipelineLayout, nullptr);
            _postPipelineLayout = VK_NULL_HANDLE;
        }
        if (_postSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(_device, _postSetLayout, nullptr);
            _postSetLayout = VK_NULL_HANDLE;
        }
        if (_postSampler != VK_NULL_HANDLE) {
            vkDestroySampler(_device, _postSampler, nullptr);
            _postSampler = VK_NULL_HANDLE;
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Pass entry points (params packed as float4 arrays — the GLSL blocks
    // declare vec4 members only, so std140 cannot introduce padding drift)
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::executeComposePass(const ComposePassParams& params)
    {
        struct alignas(16) ComposeUbo
        {
            float p0[4]; // invResX, invResY, sharpness, exposure
            float p1[4]; // tonemapMode, ssaoEnabled, bloomEnabled, bloomIntensity
            float p2[4]; // dofEnabled, dofFocusDistance, dofFocusRange, dofBlurRadius
            float p3[4]; // dofCameraNear, dofCameraFar, vignetteEnabled, vignetteInner
            float p4[4]; // vignetteOuter, vignetteCurvature, vignetteIntensity, pad
            float p5[4]; // vignetteColor rgb, pad
            float p6[4]; // fringing, gradingEnabled, brightness, contrast
            float p7[4]; // saturation, tint rgb
            float p8[4]; // enhanceEnabled, shadows, highlights, vibrance
            float p9[4]; // dehaze, midtones, lut1 intensity, lut2 intensity
            float p10[4]; // lut blend, has lut1, has lut2, pad
        } ubo{};

        // Resolution from the scene texture (the compose target matches it).
        float invResX = 0.0f, invResY = 0.0f;
        if (params.sceneTexture && params.sceneTexture->width() > 0 && params.sceneTexture->height() > 0) {
            invResX = 1.0f / static_cast<float>(params.sceneTexture->width());
            invResY = 1.0f / static_cast<float>(params.sceneTexture->height());
        }
        ubo.p0[0] = invResX;
        ubo.p0[1] = invResY;
        ubo.p0[2] = params.sharpness;
        ubo.p0[3] = params.exposure;
        ubo.p1[0] = static_cast<float>(params.toneMapping);
        ubo.p1[1] = params.ssaoTexture ? 1.0f : 0.0f;
        ubo.p1[2] = params.bloomTexture ? 1.0f : 0.0f;
        ubo.p1[3] = params.bloomIntensity;
        ubo.p2[0] = (params.dofEnabled && params.depthTexture) ? 1.0f : 0.0f;
        ubo.p2[1] = params.dofFocusDistance;
        ubo.p2[2] = params.dofFocusRange;
        ubo.p2[3] = params.dofBlurRadius;
        ubo.p3[0] = params.dofCameraNear;
        ubo.p3[1] = params.dofCameraFar;
        ubo.p3[2] = params.vignetteEnabled ? 1.0f : 0.0f;
        ubo.p3[3] = params.vignetteInner;
        ubo.p4[0] = params.vignetteOuter;
        ubo.p4[1] = params.vignetteCurvature;
        ubo.p4[2] = params.vignetteIntensity;
        ubo.p5[0] = params.vignetteColor[0];
        ubo.p5[1] = params.vignetteColor[1];
        ubo.p5[2] = params.vignetteColor[2];
        ubo.p6[0] = params.fringingIntensity;
        ubo.p6[1] = params.gradingEnabled ? 1.0f : 0.0f;
        ubo.p6[2] = params.gradingBrightness;
        ubo.p6[3] = params.gradingContrast;
        ubo.p7[0] = params.gradingSaturation;
        ubo.p7[1] = params.gradingTint[0];
        ubo.p7[2] = params.gradingTint[1];
        ubo.p7[3] = params.gradingTint[2];
        ubo.p8[0] = params.colorEnhanceEnabled ? 1.0f : 0.0f;
        ubo.p8[1] = params.colorEnhanceShadows;
        ubo.p8[2] = params.colorEnhanceHighlights;
        ubo.p8[3] = params.colorEnhanceVibrance;
        ubo.p9[0] = params.colorEnhanceDehaze;
        ubo.p9[1] = params.colorEnhanceMidtones;
        ubo.p9[2] = params.colorLUTIntensity;
        ubo.p9[3] = params.colorLUTIntensity2;
        ubo.p10[0] = params.colorLUTBlend;
        ubo.p10[1] = params.colorLUT ? 1.0f : 0.0f;
        ubo.p10[2] = params.colorLUT2 ? 1.0f : 0.0f;

        Texture* textures[6] = {params.sceneTexture, params.bloomTexture,
            params.ssaoTexture, params.depthTexture, params.colorLUT, params.colorLUT2};
        executePostPass(PostPassKind::Compose, textures, &ubo, sizeof(ubo));
    }

    void VulkanGraphicsDevice::executeSsaoPass(const SsaoPassParams& params)
    {
        struct alignas(16) SsaoUbo
        {
            float p0[4]; // aspect, invResX, invResY, randomize
            float p1[4]; // sampleCount, invSampleCount, intensity, power
            float p2[4]; // angleIncCos, angleIncSin, invRadiusSquared, minHorizonAngleSineSquared
            float p3[4]; // bias, peak2, projectionScaleRadius, pad
            float p4[4]; // cameraNear, cameraFar, pad, pad
        } ubo{};
        ubo.p0[0] = params.aspect;
        ubo.p0[1] = params.invResolutionX;
        ubo.p0[2] = params.invResolutionY;
        ubo.p0[3] = params.randomize;
        ubo.p1[0] = static_cast<float>(params.sampleCount);
        ubo.p1[1] = params.sampleCount > 0 ? 1.0f / static_cast<float>(params.sampleCount) : 0.0f;
        ubo.p1[2] = params.intensity;
        ubo.p1[3] = params.power;
        ubo.p2[0] = params.angleIncCos;
        ubo.p2[1] = params.angleIncSin;
        ubo.p2[2] = params.invRadiusSquared;
        ubo.p2[3] = params.minHorizonAngleSineSquared;
        ubo.p3[0] = params.bias;
        ubo.p3[1] = params.peak2;
        ubo.p3[2] = params.projectionScaleRadius;
        ubo.p4[0] = params.cameraNear;
        ubo.p4[1] = params.cameraFar;

        Texture* textures[6] = {params.depthTexture, nullptr, nullptr, nullptr, nullptr, nullptr};
        executePostPass(PostPassKind::Ssao, textures, &ubo, sizeof(ubo));
    }

    void VulkanGraphicsDevice::executeTAAPass(Texture* sourceTexture, Texture* historyTexture,
        Texture* depthTexture, const Matrix4& viewProjectionPrevious, const Matrix4& viewProjectionInverse,
        const std::array<float, 4>& jitters, const std::array<float, 4>& cameraParams,
        const bool highQuality, const bool historyValid)
    {
        struct alignas(16) TaaUbo
        {
            float viewProjectionPrevious[16];
            float viewProjectionInverse[16];
            float jitters[4];
            float texSizeFlags[4];   // textureSize.xy, highQuality, historyValid
            float cameraParams[4];
        } ubo{};

        static_assert(sizeof(Matrix4) == 64, "Matrix4 must be 64 bytes");
        std::memcpy(ubo.viewProjectionPrevious, &viewProjectionPrevious, sizeof(Matrix4));
        std::memcpy(ubo.viewProjectionInverse, &viewProjectionInverse, sizeof(Matrix4));
        std::memcpy(ubo.jitters, jitters.data(), sizeof(ubo.jitters));
        ubo.texSizeFlags[0] = sourceTexture ? static_cast<float>(sourceTexture->width()) : 1.0f;
        ubo.texSizeFlags[1] = sourceTexture ? static_cast<float>(sourceTexture->height()) : 1.0f;
        ubo.texSizeFlags[2] = highQuality ? 1.0f : 0.0f;
        ubo.texSizeFlags[3] = historyValid ? 1.0f : 0.0f;
        std::memcpy(ubo.cameraParams, cameraParams.data(), sizeof(ubo.cameraParams));

        Texture* textures[6] = {sourceTexture, historyTexture, depthTexture, nullptr, nullptr, nullptr};
        executePostPass(PostPassKind::Taa, textures, &ubo, sizeof(ubo));
    }

}

#endif // VISUTWIN_HAS_VULKAN
