// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanRenderPipeline.h"
#include "vulkanGraphicsDevice.h"
#include "vulkanShader.h"
#include "vulkanUtils.h"

#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/mesh.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    VulkanRenderPipeline::VulkanRenderPipeline(VulkanGraphicsDevice* device)
        : _device(device)
    {
        createLayouts();
    }

    VulkanRenderPipeline::~VulkanRenderPipeline()
    {
        VkDevice vk = _device->device();
        for (auto& [key, pipeline] : _cache) {
            vkDestroyPipeline(vk, pipeline, nullptr);
        }
        if (_pipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(vk, _pipelineLayout, nullptr);
        if (_materialSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(vk, _materialSetLayout, nullptr);
        if (_textureSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(vk, _textureSetLayout, nullptr);
    }

    void VulkanRenderPipeline::createLayouts()
    {
        VkDevice vk = _device->device();

        // Set 0: Material UBO
        VkDescriptorSetLayoutBinding materialBinding{};
        materialBinding.binding = 0;
        materialBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        materialBinding.descriptorCount = 1;
        materialBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo materialLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        materialLayoutInfo.bindingCount = 1;
        materialLayoutInfo.pBindings = &materialBinding;
        vkCreateDescriptorSetLayout(vk, &materialLayoutInfo, nullptr, &_materialSetLayout);

        // Set 1: Texture samplers (6 slots)
        std::array<VkDescriptorSetLayoutBinding, 6> texBindings{};
        for (uint32_t i = 0; i < texBindings.size(); i++) {
            texBindings[i].binding = i;
            texBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            texBindings[i].descriptorCount = 1;
            texBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo textureLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        textureLayoutInfo.bindingCount = static_cast<uint32_t>(texBindings.size());
        textureLayoutInfo.pBindings = texBindings.data();
        vkCreateDescriptorSetLayout(vk, &textureLayoutInfo, nullptr, &_textureSetLayout);

        // Push constants: 2 × mat4 = 128 bytes
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 128;

        VkDescriptorSetLayout setLayouts[] = {_materialSetLayout, _textureSetLayout};

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 2;
        layoutInfo.pSetLayouts = setLayouts;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(vk, &layoutInfo, nullptr, &_pipelineLayout);
    }

    VkPipeline VulkanRenderPipeline::get(const Primitive& primitive,
        const std::shared_ptr<VertexFormat>& vertexFormat,
        const std::shared_ptr<VulkanShader>& shader,
        const std::shared_ptr<BlendState>& blendState,
        const std::shared_ptr<DepthState>& depthState,
        CullMode cullMode,
        VkFormat colorFormat,
        VkFormat depthFormat)
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
        mix(static_cast<uint64_t>(colorFormat));
        mix(static_cast<uint64_t>(depthFormat));

        auto it = _cache.find(hash);
        if (it != _cache.end()) return it->second;

        VkPipeline pipeline = create(primitive, vertexFormat, shader,
            blendState, depthState, cullMode, colorFormat, depthFormat);
        _cache[hash] = pipeline;
        return pipeline;
    }

    VkPipeline VulkanRenderPipeline::create(const Primitive& primitive,
        const std::shared_ptr<VertexFormat>& vertexFormat,
        const std::shared_ptr<VulkanShader>& shader,
        const std::shared_ptr<BlendState>& blendState,
        const std::shared_ptr<DepthState>& depthState,
        CullMode cullMode,
        VkFormat colorFormat,
        VkFormat depthFormat)
    {
        VkDevice vk = _device->device();

        // --- Shader stages ---
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        if (shader->vertexModule() != VK_NULL_HANDLE) {
            VkPipelineShaderStageCreateInfo vert{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            vert.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vert.module = shader->vertexModule();
            vert.pName = "main";
            stages.push_back(vert);
        }
        if (shader->fragmentModule() != VK_NULL_HANDLE) {
            VkPipelineShaderStageCreateInfo frag{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            frag.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            frag.module = shader->fragmentModule();
            frag.pName = "main";
            stages.push_back(frag);
        }

        // --- Vertex input ---
        // Standard layout: pos(3f) + normal(3f) + uv0(2f) + tangent(4f) + uv1(2f) = 56 bytes
        int stride = vertexFormat ? vertexFormat->size() : 56;

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = static_cast<uint32_t>(stride);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        // Derive attribute descriptions from vertex format elements
        std::vector<VkVertexInputAttributeDescription> attributes;
        if (vertexFormat) {
            for (int i = 0; i < vertexFormat->elementCount(); i++) {
                auto& elem = vertexFormat->element(i);
                VkVertexInputAttributeDescription attr{};
                attr.location = static_cast<uint32_t>(i);
                attr.binding = 0;
                attr.offset = static_cast<uint32_t>(elem.offset);

                // Map element type + component count to VkFormat
                switch (elem.numComponents) {
                case 1:
                    attr.format = (elem.dataType == TYPE_FLOAT32) ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32_UINT;
                    break;
                case 2:
                    attr.format = VK_FORMAT_R32G32_SFLOAT;
                    break;
                case 3:
                    attr.format = VK_FORMAT_R32G32B32_SFLOAT;
                    break;
                case 4:
                    attr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                    break;
                default:
                    attr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
                    break;
                }
                attributes.push_back(attr);
            }
        } else {
            // Fallback: hardcoded standard vertex layout
            attributes = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},       // position
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},      // normal
                {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},         // uv0
                {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},   // tangent
                {4, 0, VK_FORMAT_R32G32_SFLOAT, 48},         // uv1
            };
        }

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        // --- Input assembly ---
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = vulkanMapPrimitiveType(primitive.type);
        inputAssembly.primitiveRestartEnable = VK_FALSE;

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
        rasterization.depthBiasEnable = VK_FALSE;
        rasterization.lineWidth = 1.0f;

        // --- Multisample ---
        VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // --- Depth/stencil ---
        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        if (depthState) {
            depthStencil.depthTestEnable = depthState->depthTest() ? VK_TRUE : VK_FALSE;
            depthStencil.depthWriteEnable = depthState->depthWrite() ? VK_TRUE : VK_FALSE;
        } else {
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
        }
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        // --- Color blend ---
        VkPipelineColorBlendAttachmentState blendAttachment{};
        if (blendState && blendState->enabled()) {
            blendAttachment.blendEnable = VK_TRUE;
            blendAttachment.srcColorBlendFactor = vulkanMapBlendFactor(blendState->colorSrcFactor());
            blendAttachment.dstColorBlendFactor = vulkanMapBlendFactor(blendState->colorDstFactor());
            blendAttachment.colorBlendOp = vulkanMapBlendOp(blendState->colorOp());
            blendAttachment.srcAlphaBlendFactor = vulkanMapBlendFactor(blendState->alphaSrcFactor());
            blendAttachment.dstAlphaBlendFactor = vulkanMapBlendFactor(blendState->alphaDstFactor());
            blendAttachment.alphaBlendOp = vulkanMapBlendOp(blendState->alphaOp());
        }
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;

        // --- Dynamic state ---
        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        // --- Dynamic rendering (Vulkan 1.3) ---
        VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &colorFormat;
        renderingInfo.depthAttachmentFormat = depthFormat;

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
