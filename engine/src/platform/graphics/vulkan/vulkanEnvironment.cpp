// SPDX-License-Identifier: Apache-2.0
#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanGraphicsDevice.h"

#include <array>
#include <memory>

#include "platform/graphics/texture.h"
#include "vulkanTexture.h"
#include "vulkan/vulkan_shader_bundle.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    void VulkanGraphicsDevice::renderEnvironment(Texture* target,
        Texture* sourceEquirect, Texture* sourceCubemap,
        const std::vector<EnvReprojectOp>& ops, const bool encodeRgbp,
        const bool decodeSrgb, const bool clearTarget, const bool cubemapFaces,
        const bool convolve)
    {
        if (!target || (!sourceEquirect && !sourceCubemap) ||
            (!cubemapFaces && ops.empty())) return;
        auto* dst = dynamic_cast<gpu::VulkanTexture*>(target->impl());
        auto* src2d = sourceEquirect
            ? dynamic_cast<gpu::VulkanTexture*>(sourceEquirect->impl()) : nullptr;
        auto* srcCube = sourceCubemap
            ? dynamic_cast<gpu::VulkanTexture*>(sourceCubemap->impl()) : nullptr;
        if (!dst || !dst->supportsColorAttachment()) {
            spdlog::error("Vulkan environment target is not color-renderable");
            return;
        }

        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sli.bindingCount = 2;
        sli.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(_device, &sli, nullptr, &setLayout) != VK_SUCCESS) return;

        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48};
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &push;
        vkCreatePipelineLayout(_device, &pli, nullptr, &pipelineLayout);

        auto makeModule = [&](const uint32_t* words, const size_t count) {
            VkShaderModule module = VK_NULL_HANDLE;
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = count * sizeof(uint32_t);
            info.pCode = words;
            vkCreateShaderModule(_device, &info, nullptr, &module);
            return module;
        };
        VkShaderModule vert = makeModule(vulkan_generated::kPostFullscreenVert,
            vulkan_generated::kPostFullscreenVertWordCount);
        VkShaderModule frag = makeModule(vulkan_generated::kEnvReprojectFrag,
            vulkan_generated::kEnvReprojectFragWordCount);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main"};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main"};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xf;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2; ds.pDynamicStates = dynamic;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        const VkFormat targetFormat = dst->format();
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &targetFormat;
        VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi.pNext = &rendering; gpi.stageCount = 2; gpi.pStages = stages;
        gpi.pVertexInputState = &vi; gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp; gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms; gpi.pColorBlendState = &cb;
        gpi.pDynamicState = &ds; gpi.layout = pipelineLayout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline);

        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &poolSize;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(_device, &dpi, nullptr, &pool);
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &setLayout;
        vkAllocateDescriptorSets(_device, &dai, &set);
        std::array<VkDescriptorImageInfo, 2> images{{
            {_envSampler, src2d ? src2d->imageView() : _whiteImageView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {_envSampler, srcCube ? srcCube->imageView() : _whiteCubeImageView,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        }};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set; writes[i].dstBinding = i;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1; writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(_device, 2, writes.data(), 0, nullptr);

        std::vector<VkImageView> views;
        if (cubemapFaces) {
            views.resize(6);
            for (uint32_t face = 0; face < 6; ++face) {
                VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                info.image = dst->image(); info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                info.format = dst->format();
                info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, face, 1};
                vkCreateImageView(_device, &info, nullptr, &views[face]);
            }
        } else {
            views.push_back(dst->imageView());
        }

        enqueueUpload([=](VkCommandBuffer cmd) {
            if (src2d) src2d->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (srcCube) srcCube->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            dst->transitionLayout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 1);
            const uint32_t draws = cubemapFaces ? 6u : static_cast<uint32_t>(ops.size());
            for (uint32_t i = 0; i < draws; ++i) {
                const EnvReprojectOp op = cubemapFaces
                    ? EnvReprojectOp{0, 0, static_cast<int>(target->width()),
                        static_cast<int>(target->height()), 0} : ops[i];
                VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                attachment.imageView = views[cubemapFaces ? i : 0];
                attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.loadOp = (clearTarget && (cubemapFaces || i == 0))
                    ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea.extent = {target->width(), target->height()};
                ri.layerCount = 1; ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &attachment;
                vkCmdBeginRendering(cmd, &ri);
                VkViewport viewport{0, 0, static_cast<float>(target->width()),
                    static_cast<float>(target->height()), 0, 1};
                VkRect2D scissor{{op.rectX, op.rectY},
                    {static_cast<uint32_t>(op.rectW), static_cast<uint32_t>(op.rectH)}};
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                const float roughness = convolve && draws > 1
                    ? static_cast<float>(i) / static_cast<float>(draws - 1) : 0.0f;
                const float pushData[12] = {
                    static_cast<float>(op.rectX), static_cast<float>(op.rectY),
                    static_cast<float>(op.rectW), static_cast<float>(op.rectH),
                    srcCube ? 1.0f : 0.0f, decodeSrgb ? 1.0f : 0.0f,
                    encodeRgbp ? 1.0f : 0.0f,
                    cubemapFaces ? static_cast<float>(i) : -1.0f,
                    convolve ? 1.0f : 0.0f, roughness, 0.0f, 0.0f};
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 0, 1, &set, 0, nullptr);
                vkCmdPushConstants(cmd, pipelineLayout,
                    VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushData), pushData);
                vkCmdDraw(cmd, 3, 1, 0, 0);
                vkCmdEndRendering(cmd);
            }
            if (cubemapFaces) dst->generateMipmaps(cmd, 0, 6);
            else dst->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }, [=, this] {
            for (VkImageView view : views) if (cubemapFaces && view) vkDestroyImageView(_device, view, nullptr);
            vkDestroyDescriptorPool(_device, pool, nullptr);
            vkDestroyPipeline(_device, pipeline, nullptr);
            vkDestroyShaderModule(_device, vert, nullptr);
            vkDestroyShaderModule(_device, frag, nullptr);
            vkDestroyPipelineLayout(_device, pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(_device, setLayout, nullptr);
        });
    }

    void VulkanGraphicsDevice::generateEnvReproject(const EnvReprojectPassParams& p)
    {
        renderEnvironment(p.target, p.sourceEquirect, p.sourceCubemap, p.ops,
            p.encodeRgbp, p.decodeSrgb, true, false);
    }

    void VulkanGraphicsDevice::generateEnvConvolve(const EnvConvolvePassParams& p)
    {
        std::vector<EnvReprojectOp> ops;
        for (const auto& op : p.ops)
            ops.push_back({op.rectX, op.rectY, op.rectW, op.rectH, op.seamPixels});
        renderEnvironment(p.target, p.sourceEquirect, p.sourceCubemap, ops,
            p.encodeRgbp, p.decodeSrgb, true, false, true);
    }

    void VulkanGraphicsDevice::generateEnvAtlas(const EnvAtlasBakeParams& p)
    {
        renderEnvironment(p.target, p.reprojectSourceEquirect,
            p.reprojectSourceCubemap, p.reprojectOps,
            p.encodeRgbp, p.decodeSrgb, true, false);
        std::vector<EnvReprojectOp> ops;
        for (const auto& op : p.convolveOps)
            ops.push_back({op.rectX, op.rectY, op.rectW, op.rectH, op.seamPixels});
        renderEnvironment(p.target, p.convolveSourceEquirect,
            p.convolveSourceCubemap, ops,
            p.encodeRgbp, p.decodeSrgb, false, false, true);
    }

    void VulkanGraphicsDevice::generateEquirectToCubemap(const EquirectToCubeParams& p)
    {
        renderEnvironment(p.target, p.source, nullptr, {}, false,
            p.decodeSrgb, true, true);
    }
}
#endif
