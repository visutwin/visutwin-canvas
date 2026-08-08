// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanGraphicsDevice.h"

#include <algorithm>
#include <cstring>
#include <VkBootstrap.h>
#include <SDL3/SDL_vulkan.h>

#include "vulkanIndexBuffer.h"
#include "vulkanInstanceCullPass.h"
#include "vulkanRenderPipeline.h"
#include "vulkanRenderTarget.h"
#include "vulkanShader.h"
#include "vulkanShaderCompiler.h"
#include "vulkanTexture.h"
#include "vulkanUniformRingBuffer.h"
#include "vulkanUtils.h"
#include "vulkanVertexBuffer.h"

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/shaderFeatures.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

#include "vulkan/vulkan_shader_bundle.h"

namespace visutwin::canvas
{

    void VulkanGraphicsDevice::ensureVsmBlurResources()
    {
        if (_vsmBlurPipelineLayout != VK_NULL_HANDLE ||
            _vsmBlurResourcesAttempted) {
            return;
        }
        _vsmBlurResourcesAttempted = true;

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 1;
        setInfo.pBindings = &binding;
        VkResult result = vkCreateDescriptorSetLayout(
            _device, &setInfo, nullptr, &_vsmBlurSetLayout);
        if (result != VK_SUCCESS) {
            spdlog::error(
                "VulkanGraphicsDevice: VSM blur descriptor set layout creation failed ({})",
                static_cast<int>(result));
            return;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = 32; // vec4 dirInvRes + vec4 filterParams

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &_vsmBlurSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        result = vkCreatePipelineLayout(
            _device, &layoutInfo, nullptr, &_vsmBlurPipelineLayout);
        if (result != VK_SUCCESS) {
            spdlog::error(
                "VulkanGraphicsDevice: VSM blur pipeline layout creation failed ({})",
                static_cast<int>(result));
            vkDestroyDescriptorSetLayout(
                _device, _vsmBlurSetLayout, nullptr);
            _vsmBlurSetLayout = VK_NULL_HANDLE;
            return;
        }

        auto createModule = [this](
                                const uint32_t* spirv,
                                size_t words) -> VkShaderModule {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = words * sizeof(uint32_t);
            info.pCode = spirv;
            VkShaderModule module = VK_NULL_HANDLE;
            const VkResult moduleResult =
                vkCreateShaderModule(_device, &info, nullptr, &module);
            if (moduleResult != VK_SUCCESS) {
                spdlog::error(
                    "VulkanGraphicsDevice: VSM blur shader module creation failed ({})",
                    static_cast<int>(moduleResult));
                return VK_NULL_HANDLE;
            }
            return module;
        };
        _vsmBlurVertModule = createModule(
            vulkan_generated::kVsmBlurVert,
            vulkan_generated::kVsmBlurVertWordCount);
        _vsmBlurFragModule = createModule(
            vulkan_generated::kVsmBlurFrag,
            vulkan_generated::kVsmBlurFragWordCount);
        if (_vsmBlurVertModule == VK_NULL_HANDLE ||
            _vsmBlurFragModule == VK_NULL_HANDLE) {
            if (_vsmBlurVertModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(
                    _device, _vsmBlurVertModule, nullptr);
            }
            if (_vsmBlurFragModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(
                    _device, _vsmBlurFragModule, nullptr);
            }
            vkDestroyPipelineLayout(
                _device, _vsmBlurPipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(
                _device, _vsmBlurSetLayout, nullptr);
            _vsmBlurVertModule = VK_NULL_HANDLE;
            _vsmBlurFragModule = VK_NULL_HANDLE;
            _vsmBlurPipelineLayout = VK_NULL_HANDLE;
            _vsmBlurSetLayout = VK_NULL_HANDLE;
        }
    }

    VkPipeline VulkanGraphicsDevice::getVsmBlurPipeline(const VkFormat colorFormat, const VkFormat depthFormat)
    {
        const uint64_t key = (static_cast<uint64_t>(colorFormat) << 32) |
                             static_cast<uint64_t>(depthFormat);
        if (const auto it = _vsmBlurPipelines.find(key); it != _vsmBlurPipelines.end()) {
            return it->second;
        }
        if (_vsmBlurVertModule == VK_NULL_HANDLE || _vsmBlurFragModule == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = _vsmBlurVertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = _vsmBlurFragModule;
        stages[1].pName = "main";

        // Fullscreen triangle: no vertex input.
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

        // Fullscreen blit — depth untouched even if the pass carries a depth attachment.
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
        pipelineInfo.layout = _vsmBlurPipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
            spdlog::error("VulkanGraphicsDevice: VSM blur pipeline creation failed");
        }
        _vsmBlurPipelines[key] = pipeline;
        return pipeline;
    }

    void VulkanGraphicsDevice::executeVsmBlurPass(const VsmBlurPassParams& params, const bool horizontal)
    {
        if (!_frameActive || !_dynamicRenderingActive || !params.sourceTexture) {
            return;
        }
        auto* sourceTex = static_cast<gpu::VulkanTexture*>(params.sourceTexture->impl());
        if (!sourceTex || sourceTex->imageView() == VK_NULL_HANDLE) {
            return;
        }

        ensureVsmBlurResources();

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

        VkPipeline pipeline = getVsmBlurPipeline(colorFmt, depthFmt);
        if (pipeline == VK_NULL_HANDLE) {
            return;
        }

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = sourceTex->sampler() != VK_NULL_HANDLE ? sourceTex->sampler() : _defaultSampler;
        imageInfo.imageView = sourceTex->imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const VkDescriptorSet set = getOrCreateImageDescriptorSet(
            _vsmBlurSetLayout, std::span(&imageInfo, 1));
        if (set == VK_NULL_HANDLE) {
            return;
        }

        struct { float dirInvRes[4]; float filterParams[4]; } push{};
        push.dirInvRes[0] = horizontal ? 1.0f : 0.0f;
        push.dirInvRes[1] = horizontal ? 0.0f : 1.0f;
        push.dirInvRes[2] = params.sourceInvResolutionX;
        push.dirInvRes[3] = params.sourceInvResolutionY;
        push.filterParams[0] = static_cast<float>(params.filterSize);
        push.filterParams[1] = params.tileSize;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            _vsmBlurPipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, _vsmBlurPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        // The blur pipeline replaced the material pipeline — force a rebind
        // (and full descriptor rebind via the incompatible layout) next draw.
        _currentPipeline = VK_NULL_HANDLE;
    }

    void VulkanGraphicsDevice::startRenderPass(RenderPass* renderPass)
    {
        if (!_frameActive) {
            return; // frame skipped at acquire — command buffer is not recording
        }

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        auto* offscreen = renderPass
            ? dynamic_cast<VulkanRenderTarget*>(renderPass->renderTarget().get())
            : nullptr;

        // ── Resolve attachment views, formats, extents, and clear ops ──
        const std::vector<std::shared_ptr<ColorAttachmentOps>> emptyColorOps;
        const auto& colorArrayOps = renderPass
            ? renderPass->colorArrayOps()
            : emptyColorOps;
        auto dsOps = renderPass ? renderPass->depthStencilOps() : nullptr;


        std::vector<VkRenderingAttachmentInfo> colorInfos;
        VkRenderingAttachmentInfo depthInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingAttachmentInfo stencilInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        bool hasDepth = false;
        bool hasStencil = false;
        VkExtent2D extent{};

        if (offscreen) {
            // Offscreen: transition each attachment from its current layout
            // (typically SHADER_READ_ONLY from a previous pass, or UNDEFINED
            // on first use) into the appropriate attachment-optimal layout.
            extent = offscreen->extent();

            const auto& attachments = offscreen->colorAttachments();
            for (size_t colorIndex = 0;
                 colorIndex < attachments.size(); ++colorIndex) {
                const auto& att = attachments[colorIndex];
                if (!att.texture) continue;
                const auto colorOps = colorIndex < colorArrayOps.size()
                    ? colorArrayOps[colorIndex] : nullptr;
                const uint32_t mip = static_cast<uint32_t>(offscreen->mipLevel());
                const uint32_t layer = att.texture->arrayLayers() > 1
                    ? static_cast<uint32_t>(offscreen->face()) : 0u;
                const VkImageLayout from = att.texture->layout(mip, layer);
                if (from != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                    att.texture->transitionLayout(cmd,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        mip, 1, layer, 1);
                }

                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = att.view;
                info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                if (colorOps && colorOps->clear) {
                    info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    info.clearValue.color = {{colorOps->clearValue.r, colorOps->clearValue.g,
                                              colorOps->clearValue.b, colorOps->clearValue.a}};
                } else {
                    info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                }
                colorInfos.push_back(info);
            }

            if (offscreen->hasDepthAttachment()) {
                hasDepth = true;
                const auto& da = offscreen->depthAttachment();
                hasStencil = vulkanFormatHasStencil(da.format);
                const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT |
                    (hasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);

                // Source image + source layout differ between texture-backed
                // and internally-owned depth.
                VkImage depthImg = da.texture ? da.texture->image() : da.internalImage;
                const uint32_t depthMip = static_cast<uint32_t>(offscreen->mipLevel());
                const uint32_t depthLayer = da.texture && da.texture->arrayLayers() > 1
                    ? static_cast<uint32_t>(offscreen->face()) : 0u;
                VkImageLayout fromLayout = da.texture
                    ? da.texture->layout(depthMip, depthLayer)
                    : da.currentLayout;
                if (depthImg != VK_NULL_HANDLE &&
                    fromLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                    // Omni-shadow cubemap depth carves per-face attachment views —
                    // barrier the face being rendered, not just layer 0 (the
                    // default), or faces 1-5 render in the wrong layout.
                    if (da.texture) {
                        da.texture->transitionLayout(cmd,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            depthMip, 1, depthLayer, 1);
                    } else {
                        vulkanTransitionImageLayout(cmd, depthImg,
                            fromLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            depthAspect, 0, 1, 0, 1);
                        // Internal depth — track via the RT itself.
                        const_cast<VulkanDepthAttachment&>(da).currentLayout =
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    }
                }

                depthInfo.imageView = da.view;
                depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthInfo.loadOp = (dsOps && dsOps->clearDepth)
                    ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                depthInfo.storeOp = (dsOps && dsOps->storeDepth)
                    ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depthInfo.clearValue.depthStencil = {dsOps ? dsOps->clearDepthValue : 1.0f, 0};

                if (hasStencil) {
                    stencilInfo = depthInfo;
                    stencilInfo.loadOp = (dsOps && dsOps->clearStencil)
                        ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                    stencilInfo.storeOp = (dsOps && dsOps->storeStencil)
                        ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    stencilInfo.clearValue.depthStencil.stencil =
                        static_cast<uint32_t>(dsOps ? dsOps->clearStencilValue : 0);
                }
            }
        } else {
            // Swapchain (back-buffer) path.
            extent = _swapchainExtent;
            const auto colorOps =
                colorArrayOps.empty() ? nullptr : colorArrayOps[0];

            if (_swapchainImageLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                vulkanTransitionImageLayout(cmd, _swapchainImages[_swapchainImageIndex],
                    _swapchainImageLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                _swapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            // Transition the shared depth image from its tracked layout — a
            // blanket UNDEFINED source would discard contents even when a
            // second swapchain pass in the same frame (overlays/gizmos)
            // requests LOAD_OP_LOAD on depth.
            if (_depthImageLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                vulkanTransitionImageLayout(cmd, _depthImage,
                    _depthImageLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
                _depthImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }

            VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            info.imageView = _swapchainImageViews[_swapchainImageIndex];
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (colorOps && colorOps->clear) {
                info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                info.clearValue.color = {{colorOps->clearValue.r, colorOps->clearValue.g,
                                          colorOps->clearValue.b, colorOps->clearValue.a}};
            } else {
                info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            }
            colorInfos.push_back(info);

            hasDepth = true;
            depthInfo.imageView = _depthImageView;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = (dsOps && !dsOps->clearDepth)
                ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthInfo.storeOp = (dsOps && dsOps->storeDepth)
                ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthInfo.clearValue.depthStencil = {dsOps ? dsOps->clearDepthValue : 1.0f, 0};
        }

        VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = {{0, 0}, extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size());
        renderingInfo.pColorAttachments = colorInfos.empty() ? nullptr : colorInfos.data();
        renderingInfo.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
        renderingInfo.pStencilAttachment = hasStencil ? &stencilInfo : nullptr;

        vkCmdBeginRendering(cmd, &renderingInfo);

        _activeOffscreenTarget = offscreen;
        _activeExtent = extent;
        _dynamicRenderingActive = true;
        _insideRenderPass = true;
        _currentPipeline = VK_NULL_HANDLE;
        _pushConstantsDirty = true;

        // Depth-only offscreen passes are shadow-map renders. They use the SAME
        // negative-height viewport as every other pass: the shadow sample
        // matrices (shadowMatrixPalette) bake the Metal top-left atlas
        // orientation, and the negative-height viewport stores the map in
        // exactly that orientation — so the sampling shader needs no V flip.
        // (_depthOnlyPass only drives the white-texture descriptor fallbacks.)
        _depthOnlyPass = offscreen && colorInfos.empty() && hasDepth;

        // Every pass starts with a full-target viewport/scissor — same
        // contract as the Metal backend, which resets both at encoder
        // creation.  Camera rects / gizmo viewports are applied afterwards
        // through the setViewport/setScissor overrides.
        GraphicsDevice::setViewport(0.0f, 0.0f,
            static_cast<float>(extent.width), static_cast<float>(extent.height));
        GraphicsDevice::setScissor(0, 0,
            static_cast<int>(extent.width), static_cast<int>(extent.height));
        applyViewport();
        applyScissor();
        applyDepthBias();
    }

    void VulkanGraphicsDevice::endRenderPass(RenderPass* renderPass)
    {
        if (!_dynamicRenderingActive) {
            _insideRenderPass = false;
            return;
        }

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;
        vkCmdEndRendering(cmd);
        _dynamicRenderingActive = false;

        // Offscreen attachments are usually sampled by a later pass — transition
        // each one back to SHADER_READ_ONLY so the descriptor binding that the
        // next pass writes is valid.  Layout tracking on the texture (or on the
        // RT for internal depth) makes future transitions cheap.
        if (_activeOffscreenTarget) {
            const std::vector<std::shared_ptr<ColorAttachmentOps>> emptyColorOps;
            const auto& colorArrayOps = renderPass
                ? renderPass->colorArrayOps()
                : emptyColorOps;
            const auto& attachments =
                _activeOffscreenTarget->colorAttachments();
            for (size_t colorIndex = 0;
                 colorIndex < attachments.size(); ++colorIndex) {
                const auto& att = attachments[colorIndex];
                if (!att.texture) continue;
                const auto colorOps = colorIndex < colorArrayOps.size()
                    ? colorArrayOps[colorIndex] : nullptr;
                const uint32_t mip =
                    static_cast<uint32_t>(_activeOffscreenTarget->mipLevel());
                const uint32_t layer = att.texture->arrayLayers() > 1
                    ? static_cast<uint32_t>(_activeOffscreenTarget->face()) : 0u;
                att.texture->transitionLayout(cmd,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    mip, 1, layer, 1);
                if (mip == 0 && colorOps && colorOps->genMipmaps) {
                    att.texture->generateMipmaps(cmd);
                }
            }
            if (_activeOffscreenTarget->hasDepthAttachment()) {
                const auto& da = _activeOffscreenTarget->depthAttachment();
                // Only texture-backed depth can be sampled later — it was
                // created with SAMPLED_BIT.  Internal depth lives only inside
                // the render pass and stays in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                // until the next pass that uses it (which will start with that
                // same layout).  Transitioning internal depth to
                // SHADER_READ_ONLY is illegal because its image lacks
                // SAMPLED_BIT (the image-barrier oldLayout usage rule).
                if (da.texture && da.texture->image() != VK_NULL_HANDLE) {
                    // Mirror the per-face handling in startRenderPass for
                    // layered (omni cubemap) depth textures.
                    const bool depthLayered = da.texture->arrayLayers() > 1;
                    da.texture->transitionLayout(cmd,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        static_cast<uint32_t>(_activeOffscreenTarget->mipLevel()), 1,
                        depthLayered
                            ? static_cast<uint32_t>(_activeOffscreenTarget->face()) : 0u,
                        1u);
                }
            }
            _activeOffscreenTarget = nullptr;
        }

        _insideRenderPass = false;
    }

    void VulkanGraphicsDevice::setViewport(const float x, const float y, const float w, const float h)
    {
        GraphicsDevice::setViewport(x, y, w, h);
        if (_dynamicRenderingActive) {
            applyViewport();
        }
    }

    void VulkanGraphicsDevice::setScissor(const int x, const int y, const int w, const int h)
    {
        GraphicsDevice::setScissor(x, y, w, h);
        if (_dynamicRenderingActive) {
            applyScissor();
        }
    }

    void VulkanGraphicsDevice::setDepthBias(const float depthBias, const float slopeScale, const float clamp)
    {
        _depthBiasConstant = depthBias;
        _depthBiasSlope = slopeScale;
        _depthBiasClamp = clamp;
        if (_dynamicRenderingActive) {
            applyDepthBias();
        }
    }

    void VulkanGraphicsDevice::applyViewport()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;

        const float w = vw() > 0.0f ? vw() : static_cast<float>(_activeExtent.width);
        const float h = vh() > 0.0f ? vh() : static_cast<float>(_activeExtent.height);

        // The engine uses a top-left-origin viewport (Metal convention).
        // Vulkan's normal viewport maps NDC +Y downwards; placing the origin
        // on the bottom edge of the rect and negating the height flips it so
        // projection matrices written for Metal/GL work unchanged. This applies
        // to shadow (depth-only) passes too: it stores shadow maps in the Metal
        // orientation the sample matrices bake (see startRenderPass).
        VkViewport viewport{};
        viewport.x = vx();
        viewport.y = vy() + h;
        viewport.width = w;
        viewport.height = -h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
    }

    void VulkanGraphicsDevice::applyScissor()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;

        const int64_t requestedWidth = sw() > 0
            ? static_cast<int64_t>(sw())
            : static_cast<int64_t>(_activeExtent.width);
        const int64_t requestedHeight = sh() > 0
            ? static_cast<int64_t>(sh())
            : static_cast<int64_t>(_activeExtent.height);
        const int64_t left = std::clamp<int64_t>(sx(), 0, _activeExtent.width);
        const int64_t top = std::clamp<int64_t>(sy(), 0, _activeExtent.height);
        const int64_t right = std::clamp<int64_t>(
            static_cast<int64_t>(sx()) + requestedWidth, left, _activeExtent.width);
        const int64_t bottom = std::clamp<int64_t>(
            static_cast<int64_t>(sy()) + requestedHeight, top, _activeExtent.height);

        VkRect2D scissor{};
        scissor.offset = {
            static_cast<int32_t>(left),
            static_cast<int32_t>(top)
        };
        scissor.extent = {
            static_cast<uint32_t>(right - left),
            static_cast<uint32_t>(bottom - top)
        };
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void VulkanGraphicsDevice::applyDepthBias()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;
        vkCmdSetDepthBias(cmd, _depthBiasConstant, _depthBiasClamp, _depthBiasSlope);
    }

    void VulkanGraphicsDevice::draw(const Primitive& primitive,
        const std::shared_ptr<IndexBuffer>& indexBuffer,
        int numInstances, int indirectSlot, bool first, bool last)
    {
        if (!_shader || !_dynamicRenderingActive) return;

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        auto vulkanShader = std::dynamic_pointer_cast<VulkanShader>(_shader);
        if (!vulkanShader || vulkanShader->vertexModule() == VK_NULL_HANDLE) return;

        // Geometry bindings are deliberately one-shot, matching Metal. Move
        // them into draw-local state before any fallible pipeline/descriptor
        // work so an aborted draw cannot leak stale deformation state.
        const auto paletteOffset = _pendingPaletteOffset;
        const VkDeviceSize paletteSize = _pendingPaletteSize;
        auto morphDeltaBuffer = std::move(_pendingMorphDeltaBuffer);
        const auto morphParamsOffset = _pendingMorphParamsOffset;
        const VkDeviceSize morphParamsSize = _pendingMorphParamsSize;
        _pendingPaletteOffset.reset();
        _pendingPaletteSize = 0;
        _pendingMorphParamsOffset.reset();
        _pendingMorphParamsSize = 0;
        auto particleBuffer = std::move(_pendingParticleBuffer);
        const auto particleParams = _pendingParticleParams;
        const size_t particleParamsSize = _pendingParticleParamsSize;
        _pendingParticleParamsSize = 0;
        auto splatBuffer = std::move(_pendingGSplatBuffer);
        auto splatOrderBuffer = std::move(_pendingGSplatOrderBuffer);
        auto splatShBuffer = std::move(_pendingGSplatShBuffer);
        const auto splatParams = _pendingGSplatParams;
        const size_t splatParamsSize = _pendingGSplatParamsSize;
        _pendingGSplatParamsSize = 0;

        if (first) {
            auto vf = !_vertexBuffers.empty() ? _vertexBuffers[0] : nullptr;

            // Hardware instancing: the renderer binds the per-instance buffer
            // at engine slot 5 with an isInstancing() format (same contract
            // as the Metal backend).  Scan the upper slots for it.
            const VulkanVertexBuffer* instancingVB = nullptr;
            for (size_t i = 1; i < _vertexBuffers.size(); ++i) {
                if (_vertexBuffers[i] && _vertexBuffers[i]->format() &&
                    _vertexBuffers[i]->format()->isInstancing()) {
                    instancingVB = static_cast<VulkanVertexBuffer*>(_vertexBuffers[i].get());
                    break;
                }
            }
            const auto instanceFormat = instancingVB ? instancingVB->format() : nullptr;

            // Resolve attachment formats for pipeline creation.  The pipeline
            // is keyed on these — a mismatch with the actual VkRenderingInfo
            // attachments at draw-time is rejected by validation as
            // VUID-vkCmdDrawIndexed-dynamicRenderingUnusedAttachments-08910.
            std::vector<VkFormat> colorFormats{_swapchainFormat};
            VkFormat depthFmt = _depthFormat;
            if (_activeOffscreenTarget) {
                const auto& colors = _activeOffscreenTarget->colorAttachments();
                colorFormats.clear();
                colorFormats.reserve(colors.size());
                for (const auto& color : colors) {
                    colorFormats.push_back(color.format);
                }
                depthFmt = _activeOffscreenTarget->hasDepthAttachment()
                    ? _activeOffscreenTarget->depthAttachment().format
                    : VK_FORMAT_UNDEFINED;
            }

            // The skybox is an inward-facing shell whose authored winding,
            // combined with our negative-height (Y-flipped) viewport, makes
            // its CULLFACE_FRONT cull the visible inner faces.  Render it with
            // no culling so the environment shell is always drawn, and select
            // the depth-pin skybox vertex stage.
            const bool isSkybox = _material && _material->isSkybox();
            CullMode cullMode = isSkybox ? CullMode::CULLFACE_NONE : _cullMode;

            VkPipeline pipeline = _renderPipeline->get(primitive,
                vf ? vf->format() : nullptr,
                instanceFormat,
                vulkanShader, _blendState, _depthState, cullMode,
                _stencilEnabled, _stencilFront, _stencilBack,
                colorFormats, depthFmt, isSkybox);

            if (pipeline == VK_NULL_HANDLE) {
                spdlog::error("VulkanGraphicsDevice: draw skipped because pipeline creation failed");
                return;
            }
            if (pipeline != _currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                _currentPipeline = pipeline;
                _pushConstantsDirty = true;
            }

            // Bind vertex buffer
            if (vf) {
                auto* vb = static_cast<VulkanVertexBuffer*>(vf.get());
                if (vb->buffer() != VK_NULL_HANDLE) {
                    VkBuffer buf = vb->buffer();
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
                }
            }

            // Bind per-instance buffer at binding 1 (matches the pipeline's
            // VK_VERTEX_INPUT_RATE_INSTANCE binding).
            if (instancingVB && instancingVB->buffer() != VK_NULL_HANDLE) {
                VkBuffer instBuf = instancingVB->buffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &offset);
            }
        }

        if (_stencilEnabled && (_stencilFront || _stencilBack)) {
            const auto& effectiveFront = _stencilFront ? _stencilFront : _stencilBack;
            const auto& effectiveBack = _stencilBack ? _stencilBack : _stencilFront;
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT,
                effectiveFront->reference());
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT,
                effectiveBack->reference());
        } else {
            // Pipelines declare stencil reference as dynamic even when the
            // current attachment/state does not use stencil. Define it on
            // every command buffer so a first non-stencil draw never depends
            // on state left by an earlier draw or frame.
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        }

        // Push constants (transforms)
        if (_pushConstantsDirty) {
            vkCmdPushConstants(cmd, _renderPipeline->pipelineLayout(),
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &_pushConstants);
            _pushConstantsDirty = false;
        }

        // Set 2: per-pass lighting UBO.  Packed once per frame (or whenever
        // setLightingUniforms changed it) into the ring; every draw binds the
        // same descriptor set with the cached dynamic offset.
        if (_lightingNeedsUpload) {
            const auto lightingOffset =
                allocateUniform(&_lightingUbo, sizeof(VulkanLightingUBO));
            if (!lightingOffset) {
                return;
            }
            _lightingSlotOffset = *lightingOffset;
            _lightingNeedsUpload = false;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            _renderPipeline->pipelineLayout(), 2, 1, &_lightingDescriptorSet,
            1, &_lightingSlotOffset);

        // Set 0: per-draw material UBO.  Pack MaterialUniforms (or the
        // material's custom uniform block) into the ring and bind via the
        // dynamic offset.  The shader's MaterialData block is statically used,
        // so it MUST be bound (VUID-vkCmdDrawIndexed-None-08600).
        {
            MaterialUniforms materialUniforms;
            const void* uniformData = &materialUniforms;
            size_t uniformSize = sizeof(MaterialUniforms);
            if (_material) {
                size_t customSize = 0;
                const void* customData = _material->customUniformData(customSize);
                if (customData && customSize > 0) {
                    uniformData = customData;
                    uniformSize = customSize;
                } else {
                    _material->updateUniforms(materialUniforms);
                }
            }
            const auto matOffset = allocateUniform(uniformData, uniformSize);
            if (!matOffset) {
                return;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 0, 1, &_materialDescriptorSet,
                1, &*matOffset);
        }

        // Set 1: material textures. Cache identical image/sampler tuples for
        // the lifetime of this frame slot instead of allocating per draw.
        {
            // 17/23/25 are separate images and 24 is their shared sampler, so
            // the descriptor for each carries only the half it owns.
            constexpr std::array<int, 10> materialSlots =
                {0, 1, 3, 4, 5, 17, 19, 23, 24, 25};
            const auto isSeparateImageSlot = [](const int slot) {
                return slot == 17 || slot == 23 || slot == 25;
            };
            std::array<VkDescriptorImageInfo, materialSlots.size()> imageInfos{};
            for (size_t i = 0; i < imageInfos.size(); ++i) {
                const int slot = materialSlots[i];
                imageInfos[i].sampler = (slot == 24) ? _materialExtraSampler
                    : (isSeparateImageSlot(slot) ? VK_NULL_HANDLE : _defaultSampler);
                imageInfos[i].imageView =
                    (slot == 24) ? VK_NULL_HANDLE : _whiteImageView;
                imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            if (_material) {
                std::vector<TextureSlot> texSlots;
                _material->getTextureSlots(texSlots);
                for (const auto& ts : texSlots) {
                    // The displacement map arrives on the >=100 sentinel slot
                    // that routes it to the vertex stage (mirrors Metal).
                    const int slot = (ts.slot >= 100) ? 25 : ts.slot;
                    const auto slotIt = std::find(
                        materialSlots.begin(), materialSlots.end(), slot);
                    if (slotIt == materialSlots.end() || ts.texture == nullptr) {
                        continue;
                    }
                    auto* vkTex =
                        static_cast<gpu::VulkanTexture*>(ts.texture->impl());
                    if (vkTex && vkTex->imageView() != VK_NULL_HANDLE) {
                        const size_t descriptorIndex =
                            static_cast<size_t>(slotIt - materialSlots.begin());
                        imageInfos[descriptorIndex].imageView = vkTex->imageView();
                        // Separate images must leave the sampler half null; they
                        // read through the shared sampler at binding 24.
                        if (!isSeparateImageSlot(slot) &&
                            vkTex->sampler() != VK_NULL_HANDLE) {
                            imageInfos[descriptorIndex].sampler = vkTex->sampler();
                        }
                    }
                }
            }

            // Quad passes (bloom downsample, outline extend/blend) carry no
            // material — their inputs arrive through setQuadTextureBinding.
            // Mirror the Metal backend, which binds those to fragment texture
            // slots 0..7: here the same indices are set-1 bindings, so a quad
            // shader declares its source at (set = 1, binding = 0). Only the
            // slots this layout actually has are reachable.
            for (size_t slotIndex = 0; slotIndex < materialSlots.size(); ++slotIndex) {
                const int slot = materialSlots[slotIndex];
                if (slot < 0 || slot >= static_cast<int>(quadTextureBindings().size())) {
                    continue;
                }
                Texture* quadTexture = quadTextureBinding(static_cast<size_t>(slot));
                if (!quadTexture) {
                    continue;
                }
                auto* vkTex = static_cast<gpu::VulkanTexture*>(quadTexture->impl());
                if (!vkTex || vkTex->imageView() == VK_NULL_HANDLE) {
                    continue;
                }
                imageInfos[slotIndex].imageView = vkTex->imageView();
                if (!isSeparateImageSlot(slot) && vkTex->sampler() != VK_NULL_HANDLE) {
                    imageInfos[slotIndex].sampler = vkTex->sampler();
                }
            }

            const VkDescriptorSet texSet = getOrCreateImageDescriptorSet(
                _renderPipeline->textureSetLayout(), imageInfos);
            if (texSet == VK_NULL_HANDLE) {
                return;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 1, 1, &texSet, 0, nullptr);
        }

        // Set 3: scene textures.  Binding 0 = environment atlas (or white
        // fallback) read through the dedicated clamp-to-edge env sampler.
        {
            auto resolveView = [this](Texture* tex) -> VkImageView {
                if (tex) {
                    auto* vkTex =
                        static_cast<gpu::VulkanTexture*>(tex->impl());
                    if (vkTex && vkTex->imageView() != VK_NULL_HANDLE) {
                        return vkTex->imageView();
                    }
                }
                return _whiteImageView;
            };

            // Resolve a cube view, falling back to the white cubemap so an
            // unbound omni slot reads fully lit (and never mixes a 2D view
            // into a samplerCube descriptor, which is invalid).
            auto resolveCubeView = [this](Texture* tex) -> VkImageView {
                if (tex) {
                    auto* vkTex =
                        static_cast<gpu::VulkanTexture*>(tex->impl());
                    if (vkTex && vkTex->imageView() != VK_NULL_HANDLE) {
                        return vkTex->imageView();
                    }
                }
                return _whiteCubeImageView;
            };

            // During a shadow render the shadow map is the attachment being
            // written, so bind white fallbacks instead of creating feedback.
            bool shadowIsActiveAttachment = false;
            if (_activeOffscreenTarget && _shadowMapTexture &&
                !_activeOffscreenTarget->colorAttachments().empty()) {
                shadowIsActiveAttachment =
                    _activeOffscreenTarget->colorAttachments()[0].texture ==
                    static_cast<gpu::VulkanTexture*>(
                        _shadowMapTexture->impl());
            }
            const bool hideShadowMaps =
                _depthOnlyPass || shadowIsActiveAttachment;

            std::array<VkDescriptorImageInfo, 14> sceneInfos{};
            sceneInfos[0].sampler = _envSampler;
            sceneInfos[0].imageView = resolveView(_envAtlasTexture);

            sceneInfos[1].sampler = _shadowSampler;
            if (!hideShadowMaps && _shadowMapTexture) {
                if (auto* vkShadowTex =
                        static_cast<gpu::VulkanTexture*>(
                            _shadowMapTexture->impl());
                    vkShadowTex &&
                    vkShadowTex->sampler() != VK_NULL_HANDLE) {
                    sceneInfos[1].sampler = vkShadowTex->sampler();
                }
            }
            sceneInfos[1].imageView = hideShadowMaps
                ? _whiteImageView : resolveView(_shadowMapTexture);
            sceneInfos[2].sampler = _shadowSampler;
            sceneInfos[2].imageView = _depthOnlyPass
                ? _whiteImageView : resolveView(_localShadowTexture0);
            sceneInfos[3].sampler = _shadowSampler;
            sceneInfos[3].imageView = _depthOnlyPass
                ? _whiteImageView : resolveView(_localShadowTexture1);
            sceneInfos[4].sampler = _shadowSampler;
            sceneInfos[4].imageView = _depthOnlyPass
                ? _whiteCubeImageView : resolveCubeView(_omniShadowCube0);
            sceneInfos[5].sampler = _shadowSampler;
            sceneInfos[5].imageView = _depthOnlyPass
                ? _whiteCubeImageView : resolveCubeView(_omniShadowCube1);
            // Bindings 6-11 are separate images (no sampler in the descriptor);
            // they read through the two shared samplers written at 12-13.
            sceneInfos[6].imageView = resolveCubeView(_skyboxCubeTexture);
            sceneInfos[7].imageView = resolveCubeView(_reflectionProbeTexture);
            sceneInfos[8].imageView = resolveView(_areaLightLut1);
            sceneInfos[9].imageView = resolveView(_areaLightLut2);
            sceneInfos[10].imageView = resolveView(_sceneColorGrabTexture.get());
            sceneInfos[11].imageView = resolveView(_sceneDepthGrabTexture.get());
            for (auto& sceneInfo : sceneInfos) {
                sceneInfo.imageLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            // Sampler-only descriptors: linear clamp for the LUTs/cubes/color
            // grab, nearest clamp for the depth copy.
            sceneInfos[12] = {};
            sceneInfos[12].sampler = _envSampler;
            sceneInfos[13] = {};
            sceneInfos[13].sampler = _shadowSampler;

            const VkDescriptorSet sceneSet = getOrCreateImageDescriptorSet(
                _renderPipeline->sceneSetLayout(), sceneInfos);
            if (sceneSet == VK_NULL_HANDLE) {
                return;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 3, 1, &sceneSet, 0, nullptr);
        }

        // Set 4: deformation data selected by the shared feature mask.
        const uint64_t featureMask = vulkanShader->featureMask();
        const bool usesPalette =
            (featureMask & (shaderFeatureBit(ShaderFeature::Skinning) |
                            shaderFeatureBit(ShaderFeature::DynamicBatch))) != 0;
        const bool usesMorph =
            (featureMask & shaderFeatureBit(ShaderFeature::Morphing)) != 0;
        if (usesPalette || usesMorph) {
            if ((usesPalette && (!paletteOffset || paletteSize == 0)) ||
                (usesMorph && (!morphDeltaBuffer || !morphParamsOffset ||
                               morphParamsSize == 0))) {
                spdlog::error(
                    "VulkanGraphicsDevice: draw skipped because required "
                    "palette/morph geometry state was not supplied");
                return;
            }

            const VkDescriptorSet geometrySet = allocateFrameDescriptorSet(
                _renderPipeline->geometrySetLayout());
            if (geometrySet == VK_NULL_HANDLE) {
                return;
            }

            std::array<VkDescriptorBufferInfo, 3> infos{};
            std::array<VkWriteDescriptorSet, 3> writes{};
            uint32_t writeCount = 0;
            auto appendBuffer = [&](const uint32_t binding,
                                    const VkDescriptorType type,
                                    const VkBuffer buffer,
                                    const VkDeviceSize offset,
                                    const VkDeviceSize range) {
                infos[writeCount] = {buffer, offset, range};
                auto& write = writes[writeCount];
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = geometrySet;
                write.dstBinding = binding;
                write.descriptorType = type;
                write.descriptorCount = 1;
                write.pBufferInfo = &infos[writeCount];
                ++writeCount;
            };
            if (usesPalette) {
                appendBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    _uniformRing->buffer(), *paletteOffset, paletteSize);
            }
            if (usesMorph) {
                auto* morphBuffer =
                    static_cast<VulkanVertexBuffer*>(morphDeltaBuffer.get());
                appendBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    morphBuffer->buffer(), 0, VK_WHOLE_SIZE);
                appendBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    _uniformRing->buffer(), *morphParamsOffset,
                    morphParamsSize);
            }
            vkUpdateDescriptorSets(_device, writeCount, writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 4, 1, &geometrySet, 0, nullptr);
        }

        {
            // Specialization constants do not remove statically-declared
            // descriptors from SPIR-V, so set 5 must be valid for every
            // forward draw. Non-clustered draws bind tiny zero sentinels.
            std::array<uint8_t, 144> emptyLight{};
            const uint32_t emptyCell = 0;
            const auto lightOffset = _clusterLightOffset
                ? _clusterLightOffset : allocateUniform(emptyLight.data(), emptyLight.size());
            const auto cellOffset = _clusterCellOffset
                ? _clusterCellOffset : allocateUniform(&emptyCell, sizeof(emptyCell));
            if (!lightOffset || !cellOffset) return;
            const VkDescriptorSet clusterSet = allocateFrameDescriptorSet(
                _renderPipeline->clusterSetLayout());
            if (clusterSet == VK_NULL_HANDLE) return;
            std::array<VkDescriptorBufferInfo, 2> infos{{
                {_uniformRing->buffer(), *lightOffset,
                    _clusterLightOffset ? _clusterLightSize : emptyLight.size()},
                {_uniformRing->buffer(), *cellOffset,
                    _clusterCellOffset ? _clusterCellSize : sizeof(emptyCell)},
            }};
            std::array<VkWriteDescriptorSet, 2> writes{};
            for (uint32_t i = 0; i < writes.size(); ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = clusterSet;
                writes[i].dstBinding = i;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].descriptorCount = 1;
                writes[i].pBufferInfo = &infos[i];
            }
            vkUpdateDescriptorSets(_device, writes.size(), writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 5, 1, &clusterSet, 0, nullptr);
        }

        // Set 6: dedicated GPU-driven render resources. The state is one-shot
        // and was moved above, so failed draws cannot leak it to later meshes.
        if (particleBuffer || splatBuffer) {
            const bool particleDraw = particleBuffer != nullptr;
            const void* paramsData = particleDraw
                ? static_cast<const void*>(particleParams.data())
                : static_cast<const void*>(splatParams.data());
            const size_t paramsSize = particleDraw
                ? particleParamsSize : splatParamsSize;
            auto paramsOffset = allocateUniform(paramsData, paramsSize);
            auto primary = std::dynamic_pointer_cast<VulkanVertexBuffer>(
                particleDraw ? particleBuffer : splatBuffer);
            auto order = std::dynamic_pointer_cast<VulkanVertexBuffer>(splatOrderBuffer);
            auto sh = std::dynamic_pointer_cast<VulkanVertexBuffer>(splatShBuffer);
            if (!paramsOffset || !primary || !primary->buffer() ||
                (!particleDraw && (!order || !order->buffer()))) {
                return;
            }
            const VkDescriptorSet gpuSet = allocateFrameDescriptorSet(
                _renderPipeline->gpuDrivenSetLayout());
            if (gpuSet == VK_NULL_HANDLE) return;
            std::array<VkDescriptorBufferInfo, 4> infos{};
            infos[0] = {primary->buffer(), 0, VK_WHOLE_SIZE};
            if (order) infos[1] = {order->buffer(), 0, VK_WHOLE_SIZE};
            if (sh) infos[2] = {sh->buffer(), 0, VK_WHOLE_SIZE};
            infos[3] = {_uniformRing->buffer(), *paramsOffset, paramsSize};
            std::array<VkWriteDescriptorSet, 4> writes{};
            uint32_t writeCount = 0;
            const auto addWrite = [&](uint32_t binding, VkDescriptorType type) {
                auto& write = writes[writeCount++];
                write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = gpuSet;
                write.dstBinding = binding;
                write.descriptorCount = 1;
                write.descriptorType = type;
                write.pBufferInfo = &infos[binding];
            };
            addWrite(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            if (!particleDraw) {
                addWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                // Current Vulkan splat variant evaluates SH0 color. Keep the
                // SH buffer in state so higher-band evaluation can be enabled
                // without changing the public binding contract.
                if (sh) addWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            }
            addWrite(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            vkUpdateDescriptorSets(_device, writeCount, writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 6, 1, &gpuSet, 0, nullptr);
        }

        // Draw
        if (indexBuffer) {
            auto* ib = static_cast<VulkanIndexBuffer*>(indexBuffer.get());
            if (ib->buffer() != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(cmd, ib->buffer(), 0, ib->indexType());
                if (indirectSlot >= 0 && _indirectDrawBuffer != VK_NULL_HANDLE) {
                    vkCmdDrawIndexedIndirect(cmd, _indirectDrawBuffer,
                        static_cast<VkDeviceSize>(indirectSlot) *
                            sizeof(VkDrawIndexedIndirectCommand),
                        1, sizeof(VkDrawIndexedIndirectCommand));
                    _indirectDrawBuffer = VK_NULL_HANDLE;
                } else {
                    vkCmdDrawIndexed(cmd, primitive.count, numInstances,
                        primitive.base, primitive.baseVertex, 0);
                }
            }
        } else {
            if (indirectSlot >= 0 && _indirectDrawBuffer != VK_NULL_HANDLE) {
                vkCmdDrawIndirect(cmd, _indirectDrawBuffer,
                    static_cast<VkDeviceSize>(indirectSlot) *
                        sizeof(VkDrawIndirectCommand),
                    1, sizeof(VkDrawIndirectCommand));
                _indirectDrawBuffer = VK_NULL_HANDLE;
            } else {
                vkCmdDraw(cmd, primitive.count, numInstances, primitive.base, 0);
            }
        }

        recordDrawCall();

        if (last) {
            clearVertexBuffer();
            _currentPipeline = VK_NULL_HANDLE;
        }
    }

    void VulkanGraphicsDevice::grabSceneColor(RenderTarget* source)
    {
        if (!_frameActive || _dynamicRenderingActive) return;
        Texture* sourceTexture = source && source->colorBufferCount() > 0
            ? source->getColorBuffer(0) : nullptr;
        auto* src = sourceTexture
            ? dynamic_cast<gpu::VulkanTexture*>(sourceTexture->impl()) : nullptr;
        const uint32_t sourceWidth = sourceTexture
            ? sourceTexture->width() : _swapchainExtent.width;
        const uint32_t sourceHeight = sourceTexture
            ? sourceTexture->height() : _swapchainExtent.height;
        if (!src && _swapchainImageIndex >= _swapchainImages.size()) return;
        if (!_sceneColorGrabTexture ||
            _sceneColorGrabTexture->width() != sourceWidth ||
            _sceneColorGrabTexture->height() != sourceHeight) {
            TextureOptions options;
            options.name = "sceneColorGrab";
            options.width = sourceWidth;
            options.height = sourceHeight;
            options.format = sourceTexture
                ? sourceTexture->format() : PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = true;
            _sceneColorGrabTexture = std::make_shared<Texture>(this, options);
            _sceneColorGrabTexture->upload();
        }
        auto* dst = dynamic_cast<gpu::VulkanTexture*>(
            _sceneColorGrabTexture->impl());
        if (!dst) return;
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;
        VkImage sourceImage = src ? src->image() : _swapchainImages[_swapchainImageIndex];
        if (src) {
            src->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        } else {
            vulkanTransitionImageLayout(cmd, sourceImage, _swapchainImageLayout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            _swapchainImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        dst->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {sourceWidth, sourceHeight, 1};
        vkCmdCopyImage(cmd, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (src) {
            src->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
        } else {
            vulkanTransitionImageLayout(cmd, sourceImage, _swapchainImageLayout,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            _swapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        dst->generateMipmaps(cmd);
        setSceneColorMap(_sceneColorGrabTexture.get());
    }

    void VulkanGraphicsDevice::grabSceneDepth(RenderTarget* source)
    {
        if (!_frameActive || _dynamicRenderingActive) return;
        Texture* sourceTexture = source ? source->depthBuffer() : nullptr;
        auto* src = sourceTexture
            ? dynamic_cast<gpu::VulkanTexture*>(sourceTexture->impl()) : nullptr;
        if (!sourceTexture && _depthImage == VK_NULL_HANDLE) return;
        const uint32_t sourceWidth = sourceTexture
            ? sourceTexture->width() : _swapchainExtent.width;
        const uint32_t sourceHeight = sourceTexture
            ? sourceTexture->height() : _swapchainExtent.height;
        if (!_sceneDepthGrabTexture ||
            _sceneDepthGrabTexture->width() != sourceWidth ||
            _sceneDepthGrabTexture->height() != sourceHeight) {
            TextureOptions options;
            options.name = "sceneDepthGrab";
            options.width = sourceWidth;
            options.height = sourceHeight;
            options.format = sourceTexture
                ? sourceTexture->format() : PixelFormat::PIXELFORMAT_DEPTH;
            options.mipmaps = false;
            _sceneDepthGrabTexture = std::make_shared<Texture>(this, options);
            _sceneDepthGrabTexture->upload();
        }
        auto* dst = dynamic_cast<gpu::VulkanTexture*>(
            _sceneDepthGrabTexture->impl());
        if (!dst) return;
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;
        VkImage sourceImage = src ? src->image() : _depthImage;
        if (src) {
            src->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        } else {
            vulkanTransitionImageLayout(cmd, sourceImage, _depthImageLayout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            _depthImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        dst->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.extent = {sourceWidth, sourceHeight, 1};
        vkCmdCopyImage(cmd, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (src) {
            src->transitionLayout(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                0, 1, 0, 1);
        } else {
            vulkanTransitionImageLayout(cmd, sourceImage, _depthImageLayout,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_DEPTH_BIT);
            _depthImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        // The copy is read as an ordinary sampled texture (its view carries the
        // colour aspect), so it must end in the generic shader-read layout —
        // the depth-specific one would not match the descriptor.
        dst->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        setSceneDepthGrabMap(_sceneDepthGrabTexture.get());
    }

    void VulkanGraphicsDevice::generateCubemapMips(Texture* cubemap)
    {
        if (!_frameActive || _dynamicRenderingActive || !cubemap) return;
        auto* texture = dynamic_cast<gpu::VulkanTexture*>(cubemap->impl());
        if (texture) {
            texture->generateMipmaps(_frames[_frameIndex].commandBuffer, 0, 6);
        }
    }

    void VulkanGraphicsDevice::setTransformUniforms(
        const Matrix4& viewProjection, const Matrix4& model)
    {
        // Matrix4 is alignas(16) and its union always occupies exactly 64
        // bytes of column-major float matrix data — regardless of whether the
        // build uses SSE / NEON / Apple SIMD / scalar storage.  Copy the whole
        // struct as raw bytes; it lands as 16 floats column-major.
        static_assert(sizeof(Matrix4) == 64, "Matrix4 must be 64 bytes");
        memcpy(_pushConstants.viewProjection, &viewProjection, sizeof(Matrix4));
        memcpy(_pushConstants.model, &model, sizeof(Matrix4));
        _pushConstantsDirty = true;
    }

    void VulkanGraphicsDevice::setDynamicBatchPalette(
        const void* data, const size_t size)
    {
        _pendingPaletteOffset.reset();
        _pendingPaletteSize = 0;
        if (!data || size == 0) {
            return;
        }
        _pendingPaletteOffset = allocateUniform(data, size);
        if (_pendingPaletteOffset) {
            _pendingPaletteSize = size;
        }
    }

    void VulkanGraphicsDevice::setMorphState(
        const std::shared_ptr<VertexBuffer>& deltaBuffer,
        const void* params, const size_t paramsSize)
    {
        _pendingMorphDeltaBuffer.reset();
        _pendingMorphParamsOffset.reset();
        _pendingMorphParamsSize = 0;
        if (!deltaBuffer || !params || paramsSize == 0) {
            return;
        }
        const auto offset = allocateUniform(params, paramsSize);
        if (!offset) {
            return;
        }
        _pendingMorphDeltaBuffer = deltaBuffer;
        _pendingMorphParamsOffset = offset;
        _pendingMorphParamsSize = paramsSize;
    }

    void VulkanGraphicsDevice::setClusterBuffers(
        const void* lightData, const size_t lightSize,
        const void* cellData, const size_t cellSize)
    {
        _clusterLightOffset.reset();
        _clusterCellOffset.reset();
        _clusterLightSize = _clusterCellSize = 0;
        if (!lightData || lightSize == 0 || !cellData || cellSize == 0) return;
        _clusterLightOffset = allocateUniform(lightData, lightSize);
        std::vector<uint32_t> expandedCells(cellSize);
        const auto* bytes = static_cast<const uint8_t*>(cellData);
        for (size_t i = 0; i < cellSize; ++i) expandedCells[i] = bytes[i];
        _clusterCellOffset = allocateUniform(expandedCells.data(),
            expandedCells.size() * sizeof(uint32_t));
        if (_clusterLightOffset && _clusterCellOffset) {
            _clusterLightSize = lightSize;
            _clusterCellSize = expandedCells.size() * sizeof(uint32_t);
        } else {
            _clusterLightOffset.reset();
            _clusterCellOffset.reset();
        }
    }

    void VulkanGraphicsDevice::setClusterGridParams(
        const float* boundsMin, const float* boundsRange,
        const float* cellsCountByBoundsSize, const int cellsX,
        const int cellsY, const int cellsZ, const int maxLightsPerCell,
        const int numClusteredLights)
    {
        for (int i = 0; i < 3; ++i) {
            _lightingUbo.clusterBoundsMin[i] = boundsMin ? boundsMin[i] : 0.0f;
            _lightingUbo.clusterBoundsRange[i] = boundsRange ? boundsRange[i] : 0.0f;
            _lightingUbo.clusterCellsCountByBoundsSize[i] =
                cellsCountByBoundsSize ? cellsCountByBoundsSize[i] : 0.0f;
        }
        _lightingUbo.clusterParams[0] = std::max(cellsX, 0);
        _lightingUbo.clusterParams[1] = std::max(cellsY, 0);
        _lightingUbo.clusterParams[2] = std::max(cellsZ, 0);
        _lightingUbo.clusterParams[3] = std::max(maxLightsPerCell, 0);
        _lightingUbo.clusterParams2[0] = std::max(numClusteredLights, 0);
        _lightingNeedsUpload = true;
    }

    void VulkanGraphicsDevice::setLightingUniforms(const Color& ambientColor,
        const std::vector<GpuLightData>& lights, const Vector3& cameraPosition,
        bool enableNormalMaps, float exposure, const FogParams& fogParams,
        const ShadowParams& shadowParams, int toneMapping,
        const Vector3* ambientSH, const Matrix4* viewProjection)
    {
        if (ambientSH) {
            for (size_t i = 0; i < 9; ++i) {
                _lightingUbo.ambientSH[i][0] = ambientSH[i].getX();
                _lightingUbo.ambientSH[i][1] = ambientSH[i].getY();
                _lightingUbo.ambientSH[i][2] = ambientSH[i].getZ();
                _lightingUbo.ambientSH[i][3] = 0.0f;
            }
        } else {
            std::memset(_lightingUbo.ambientSH, 0,
                sizeof(_lightingUbo.ambientSH));
        }
        // SSR projects each marched world position to screen UV with this.
        // Packed column-major for the GLSL mat4, mirroring MetalUniformBinder.
        if (viewProjection) {
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    _lightingUbo.viewProjection[col * 4 + row] =
                        viewProjection->getElement(row, col);
                }
            }
        } else {
            std::memset(_lightingUbo.viewProjection, 0,
                sizeof(_lightingUbo.viewProjection));
        }
        // Grab availability, tracked separately: dynamic refraction needs only
        // the colour grab, while the SSR march needs the depth copy too. Without
        // them the shader would sample the 1×1 white fallbacks.
        _lightingUbo.cameraNearFar[2] = _sceneColorGrabTexture ? 1.0f : 0.0f;
        _lightingUbo.cameraNearFar[3] = _sceneDepthGrabTexture ? 1.0f : 0.0f;

        // Directional cascaded shadows.  The cascade matrices, split distances,
        // and parameters all come straight from the renderer's ShadowParams;
        // the shadow map texture is bound at set 3 in draw().  Only directional
        // (CSM) shadows are wired here — local light shadows come later.
        const bool shadowsOn = shadowParams.enabled && shadowParams.shadowMap != nullptr;
        _shadowMapTexture = shadowsOn ? shadowParams.shadowMap : nullptr;
        if (shadowsOn) {
            std::memcpy(_lightingUbo.shadowMatrices, shadowParams.shadowMatrixPalette,
                sizeof(_lightingUbo.shadowMatrices));
            std::memcpy(_lightingUbo.shadowCascadeDistances, shadowParams.shadowCascadeDistances,
                sizeof(_lightingUbo.shadowCascadeDistances));
        }
        // 0 = off, 1 = PCF depth compare, 2 = EVSM moments (Chebyshev).
        _lightingUbo.shadowParams[0]  = shadowsOn ? (shadowParams.vsm ? 2.0f : 1.0f) : 0.0f;
        _lightingUbo.shadowParams[1]  = static_cast<float>(shadowParams.numCascades);
        _lightingUbo.shadowParams[2]  = shadowParams.bias;
        _lightingUbo.shadowParams[3]  = shadowParams.strength;
        _lightingUbo.shadowParams2[0] = shadowParams.normalBias;
        _lightingUbo.shadowParams2[1] = shadowParams.cascadeBlend;
        _lightingUbo.shadowParams2[2] = static_cast<float>(toneMapping);
        _lightingUbo.shadowParams2[3] = enableNormalMaps ? 1.0f : 0.0f;

        // Directional PCSS.  The shader reads these only when specialized with
        // VT_FEATURE_PCSS_SHADOWS, which the renderer enables from the same
        // shadow type — mirrors MetalUniformBinder.
        _lightingUbo.pcssParams[0] = static_cast<float>(shadowParams.pcssSamples);
        _lightingUbo.pcssParams[1] = static_cast<float>(shadowParams.pcssBlockerSamples);
        _lightingUbo.pcssParams[2] = shadowParams.penumbraSize;
        _lightingUbo.pcssParams[3] = shadowParams.penumbraFalloff;
        std::memcpy(_lightingUbo.pcssCascadeRadii, shadowParams.pcssCascadeRadii,
            sizeof(_lightingUbo.pcssCascadeRadii));
        std::memcpy(_lightingUbo.pcssCascadeDepthRanges, shadowParams.pcssCascadeDepthRanges,
            sizeof(_lightingUbo.pcssCascadeDepthRanges));

        // Local light shadows (spot 2D + omni cubemap), up to 2 casters.  Each
        // light's coneParams[3] carries its slot index (set in the light loop
        // below).  Reset the texture pointers every frame; only active slots
        // rebind them.  Matches MetalUniformBinder::packLocalShadow.
        _localShadowTexture0 = nullptr;
        _localShadowTexture1 = nullptr;
        _omniShadowCube0 = nullptr;
        _omniShadowCube1 = nullptr;
        for (int i = 0; i < ShadowParams::kMaxLocalShadows; ++i) {
            float* matDst    = (i == 0) ? _lightingUbo.localShadowMatrix0 : _lightingUbo.localShadowMatrix1;
            float* paramsDst = (i == 0) ? _lightingUbo.localShadowParams0 : _lightingUbo.localShadowParams1;
            float* omniDst   = (i == 0) ? _lightingUbo.omniShadowParams0  : _lightingUbo.omniShadowParams1;
            float* pcssDst   = (i == 0) ? _lightingUbo.localShadowPcss0   : _lightingUbo.localShadowPcss1;

            if (i >= shadowParams.localShadowCount) {
                std::memset(matDst, 0, 16 * sizeof(float));
                paramsDst[0] = 0.0001f; paramsDst[1] = 0.0f; paramsDst[2] = 1.0f; paramsDst[3] = 0.0f;
                // Clear the search area so a slot freed this frame cannot leave
                // the shader running PCSS against a stale radius.
                pcssDst[0] = 0.0f;
                continue;
            }

            const ShadowParams::LocalShadow& ls = shadowParams.localShadows[i];
            if (ls.isOmni) {
                // Omni: bind cubemap, pack [near, far, bias, intensity].  Far is
                // stashed in VP[0][0] by the renderer; the shader bias is a fixed
                // small secondary guard (polygon offset is the primary defence).
                Texture*& cube = (i == 0) ? _omniShadowCube0 : _omniShadowCube1;
                cube = ls.shadowMap;
                omniDst[0] = 0.01f;
                omniDst[1] = ls.viewProjection.getElement(0, 0);
                omniDst[2] = 0.001f;
                omniDst[3] = ls.intensity;
                std::memset(matDst, 0, 16 * sizeof(float));
            } else {
                // Spot: bind 2D depth map, pack the transposed VP matrix in the
                // column-major order the GLSL mat4 expects (mirrors Metal).
                Texture*& tex = (i == 0) ? _localShadowTexture0 : _localShadowTexture1;
                tex = ls.shadowMap;
                for (int col = 0; col < 4; ++col) {
                    for (int row = 0; row < 4; ++row) {
                        matDst[col * 4 + row] = ls.viewProjection.getElement(row, col);
                    }
                }
            }
            paramsDst[0] = ls.bias;
            paramsDst[1] = ls.normalBias;
            paramsDst[2] = ls.intensity;
            paramsDst[3] = ls.isOmni ? 1.0f : 0.0f;

            // Local PCSS: a non-zero search area switches this slot to the
            // contact-hardening path at runtime (no extra shader variant).
            pcssDst[0] = ls.pcssSearchArea;
            pcssDst[1] = ls.nearClip;
            pcssDst[2] = ls.farClip;
            pcssDst[3] = 0.0f;
        }

        // Ambient is authored in sRGB; shade in linear space like the Metal path.
        Color ambientLinear;
        ambientLinear.linear(&ambientColor);
        _lightingUbo.ambient[0] = ambientLinear.r;
        _lightingUbo.ambient[1] = ambientLinear.g;
        _lightingUbo.ambient[2] = ambientLinear.b;
        _lightingUbo.ambient[3] = 0.0f;

        _lightingUbo.cameraPosExposure[0] = cameraPosition.getX();
        _lightingUbo.cameraPosExposure[1] = cameraPosition.getY();
        _lightingUbo.cameraPosExposure[2] = cameraPosition.getZ();
        _lightingUbo.cameraPosExposure[3] = exposure;

        constexpr uint32_t kMaxLights = 8;
        const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(lights.size()), kMaxLights);
        _lightingUbo.lightCount[0] = count;

        for (uint32_t i = 0; i < kMaxLights; ++i) {
            VulkanGpuLight& dst = _lightingUbo.lights[i];
            if (i >= count) {
                dst = VulkanGpuLight{};
                dst.colorIntensity[3] = 0.0f;  // zero intensity → contributes nothing
                continue;
            }
            const GpuLightData& src = lights[i];
            Color lightLinear;
            lightLinear.linear(&src.color);

            dst.positionRange[0] = src.position.getX();
            dst.positionRange[1] = src.position.getY();
            dst.positionRange[2] = src.position.getZ();
            dst.positionRange[3] = src.range;

            dst.directionType[0] = src.direction.getX();
            dst.directionType[1] = src.direction.getY();
            dst.directionType[2] = src.direction.getZ();
            dst.directionType[3] =
                static_cast<float>(static_cast<uint32_t>(src.type));

            dst.colorIntensity[0] = lightLinear.r;
            dst.colorIntensity[1] = lightLinear.g;
            dst.colorIntensity[2] = lightLinear.b;
            dst.colorIntensity[3] = src.intensity;

            dst.coneParams[0] = src.innerConeCos;
            dst.coneParams[1] = src.outerConeCos;
            dst.coneParams[2] = src.falloffModeLinear ? 1.0f : 0.0f;
            // Local shadow slot: -1 = no shadow, 0/1 = local caster (indexes
            // localShadowMatrix/Params + the spot/omni depth map bindings).
            dst.coneParams[3] = src.castShadows
                ? static_cast<float>(src.shadowMapIndex)
                : -1.0f;
            // Area lights never cast shadows, so the shadow-slot component is
            // free to carry the LTC shape (0=rect, 1=disk, 2=sphere) — the same
            // packing the Metal path uses.
            if (src.type == GpuLightType::AreaRect) {
                dst.coneParams[3] = static_cast<float>(src.areaShape);
            }
            dst.areaRightHalfWidth[0] = src.areaRight.getX();
            dst.areaRightHalfWidth[1] = src.areaRight.getY();
            dst.areaRightHalfWidth[2] = src.areaRight.getZ();
            dst.areaRightHalfWidth[3] = src.areaHalfWidth;
            const Vector3 areaUp = src.direction.cross(src.areaRight).normalized();
            dst.areaUpHalfHeight[0] = areaUp.getX();
            dst.areaUpHalfHeight[1] = areaUp.getY();
            dst.areaUpHalfHeight[2] = areaUp.getZ();
            dst.areaUpHalfHeight[3] = src.areaHalfHeight;
        }

        Color fogLinear;
        fogLinear.linear(&fogParams.color);
        _lightingUbo.fogColorDensity[0] = fogLinear.r;
        _lightingUbo.fogColorDensity[1] = fogLinear.g;
        _lightingUbo.fogColorDensity[2] = fogLinear.b;
        _lightingUbo.fogColorDensity[3] = fogParams.density;
        _lightingUbo.fogStartEndType[0] = fogParams.start;
        _lightingUbo.fogStartEndType[1] = fogParams.end;
        _lightingUbo.fogStartEndType[2] = fogParams.enabled ? 1.0f : 0.0f;
        _lightingUbo.fogStartEndType[3] = 0.0f;

        // Re-upload into the ring on the next draw.
        _lightingNeedsUpload = true;
    }

    void VulkanGraphicsDevice::setReflectionProbeUniforms(
        Texture* cubemap, const Vector3& boxMin, const Vector3& boxMax,
        const bool boxProjection, const float intensity, const float maxLod)
    {
        _reflectionProbeTexture = cubemap;
        const Vector3 position = (boxMin + boxMax) * 0.5f;
        const Vector3 values[3] = {boxMin, boxMax, position};
        float* destinations[3] = {
            _lightingUbo.reflectionProbeBoxMin,
            _lightingUbo.reflectionProbeBoxMax,
            _lightingUbo.reflectionProbePosition};
        for (int v = 0; v < 3; ++v) {
            destinations[v][0] = values[v].getX();
            destinations[v][1] = values[v].getY();
            destinations[v][2] = values[v].getZ();
        }
        _lightingUbo.reflectionProbeParams[0] = boxProjection ? 1.0f : 0.0f;
        _lightingUbo.reflectionProbeParams[1] = intensity;
        _lightingUbo.reflectionProbeParams[2] = maxLod;
        _lightingNeedsUpload = true;
    }

    void VulkanGraphicsDevice::setEnvironmentUniforms(
        Texture* envAtlas, float skyboxIntensity, float skyboxMip,
        const Vector3& skyDomeCenter, bool isDome, Texture* skyboxCubeMap)
    {
        _envAtlasTexture = envAtlas;
        _skyboxCubeTexture = skyboxCubeMap;

        // skyParams2: xyz = dome center, w = flags (bit0 cubemap, bit1 dome).
        _lightingUbo.skyParams2[0] = skyDomeCenter.getX();
        _lightingUbo.skyParams2[1] = skyDomeCenter.getY();
        _lightingUbo.skyParams2[2] = skyDomeCenter.getZ();
        _lightingUbo.skyParams2[3] = static_cast<float>(
            (skyboxCubeMap ? 1u : 0u) | (isDome ? 2u : 0u));

        _lightingUbo.envParams[0] = skyboxIntensity;
        _lightingUbo.envParams[1] = envAtlas ? 1.0f : 0.0f;
        if (envAtlas) {
            switch (envAtlas->encoding()) {
            case TextureEncoding::RGBP:
                _lightingUbo.envParams[2] = static_cast<float>(VulkanEnvEncoding::Rgbp);
                break;
            case TextureEncoding::RGBM:
                _lightingUbo.envParams[2] = static_cast<float>(VulkanEnvEncoding::Rgbm);
                break;
            default:
                _lightingUbo.envParams[2] = static_cast<float>(VulkanEnvEncoding::Srgb);
                break;
            }
        }
        _lightingUbo.envParams[3] = skyboxMip;

        _lightingNeedsUpload = true;
    }
}

#endif // VISUTWIN_HAS_VULKAN
