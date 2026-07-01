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
        if (_lightingSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(vk, _lightingSetLayout, nullptr);
        if (_sceneSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(vk, _sceneSetLayout, nullptr);
    }

    void VulkanRenderPipeline::createLayouts()
    {
        VkDevice vk = _device->device();

        // Set 0: per-draw Material UBO (dynamic — one descriptor set, the ring
        // buffer offset varies per draw).
        VkDescriptorSetLayoutBinding materialBinding{};
        materialBinding.binding = 0;
        materialBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
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

        // Set 2: per-pass lighting/environment UBO (dynamic).
        VkDescriptorSetLayoutBinding lightingBinding{};
        lightingBinding.binding = 0;
        lightingBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        lightingBinding.descriptorCount = 1;
        lightingBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo lightingLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lightingLayoutInfo.bindingCount = 1;
        lightingLayoutInfo.pBindings = &lightingBinding;
        vkCreateDescriptorSetLayout(vk, &lightingLayoutInfo, nullptr, &_lightingSetLayout);

        // Set 3: per-pass scene textures.  Binding 0 = environment atlas
        // (equirectangular IBL + skybox source), binding 1 = directional
        // cascaded shadow-map depth atlas.
        std::array<VkDescriptorSetLayoutBinding, 2> sceneBindings{};
        for (uint32_t i = 0; i < sceneBindings.size(); ++i) {
            sceneBindings[i].binding = i;
            sceneBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            sceneBindings[i].descriptorCount = 1;
            sceneBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo sceneLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sceneLayoutInfo.bindingCount = static_cast<uint32_t>(sceneBindings.size());
        sceneLayoutInfo.pBindings = sceneBindings.data();
        vkCreateDescriptorSetLayout(vk, &sceneLayoutInfo, nullptr, &_sceneSetLayout);

        // Push constants: 2 × mat4 = 128 bytes
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 128;

        VkDescriptorSetLayout setLayouts[] = {
            _materialSetLayout, _textureSetLayout, _lightingSetLayout, _sceneSetLayout};

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 4;
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
        VkFormat depthFormat,
        uint32_t instanceStride,
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
        mix(static_cast<uint64_t>(colorFormat));
        mix(static_cast<uint64_t>(depthFormat));
        mix(static_cast<uint64_t>(instanceStride));
        mix(isSkybox ? 1ull : 0ull);

        auto it = _cache.find(hash);
        if (it != _cache.end()) return it->second;

        VkPipeline pipeline = create(primitive, vertexFormat, shader,
            blendState, depthState, cullMode, colorFormat, depthFormat, instanceStride, isSkybox);
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
        VkFormat depthFormat,
        uint32_t instanceStride,
        bool isSkybox)
    {
        VkDevice vk = _device->device();

        // Use the instanced vertex stage only when the draw carries a
        // per-instance buffer AND the shader provides that variant.
        const bool instanced = instanceStride > 0 &&
            shader->instancedVertexModule() != VK_NULL_HANDLE;
        const bool useSky = isSkybox && shader->skyVertexModule() != VK_NULL_HANDLE;

        // --- Shader stages ---
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        VkShaderModule vertModule = useSky
            ? shader->skyVertexModule()
            : (instanced ? shader->instancedVertexModule() : shader->vertexModule());
        if (vertModule != VK_NULL_HANDLE) {
            VkPipelineShaderStageCreateInfo vert{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            vert.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vert.module = vertModule;
            vert.pName = "main";
            stages.push_back(vert);
        }
        // Depth-only passes (shadow maps: colorFormat == UNDEFINED) omit the
        // fragment stage entirely.  Depth is written from rasterization, and a
        // fragment shader that declares a colour output with no colour
        // attachment is a MoltenVK hazard that silently drops depth writes.
        const bool depthOnly = colorFormat == VK_FORMAT_UNDEFINED;
        if (!depthOnly && shader->fragmentModule() != VK_NULL_HANDLE) {
            VkPipelineShaderStageCreateInfo frag{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            frag.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            frag.module = shader->fragmentModule();
            frag.pName = "main";
            stages.push_back(frag);
        }

        // --- Vertex input ---
        // The codebase currently uses a single fixed interleaved layout:
        //   pos(3f) + normal(3f) + uv0(2f) + tangent(4f) + uv1(2f) = 56 bytes
        // VertexFormat does not yet expose its elements (the Metal layout
        // builder also stubs the dynamic path), so we hardcode the standard
        // attribute set here.  When VertexFormat grows an element-iteration
        // API, replace the body below with a generated descriptor list.
        const int stride = vertexFormat ? vertexFormat->size() : 56;

        std::vector<VkVertexInputBindingDescription> bindings;
        bindings.push_back({0, static_cast<uint32_t>(stride), VK_VERTEX_INPUT_RATE_VERTEX});

        std::vector<VkVertexInputAttributeDescription> attributes = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},       // position
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},      // normal
            {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},         // uv0
            {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},   // tangent
            {4, 0, VK_FORMAT_R32G32_SFLOAT, 48},         // uv1
        };

        if (instanced) {
            // Binding 1: per-instance data — column-major mat4 occupies the
            // first 64 bytes (4 × vec4 at locations 5-8).  Any trailing data
            // in the instance stride (e.g. a 16-byte RGBA color) is skipped
            // by the attribute layout until the shader consumes it.
            bindings.push_back({1, instanceStride, VK_VERTEX_INPUT_RATE_INSTANCE});
            attributes.push_back({5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
            attributes.push_back({6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16});
            attributes.push_back({7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32});
            attributes.push_back({8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48});
        }

        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
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
        // Shadow / depth-only passes render double-sided: for closed meshes the
        // nearest (light-facing) surface wins the depth test anyway, and this
        // sidesteps winding ambiguity from the negative-height viewport.
        rasterization.cullMode = depthOnly ? VK_CULL_MODE_NONE : vulkanMapCullMode(cullMode);
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
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        // --- Color blend ---
        // colorFormat == UNDEFINED means depth-only (e.g. shadow map pass).
        // The pipeline must have zero colour attachments to match the render
        // pass attached at draw time (VUID-vkCmdDrawIndexed-colorAttachmentCount-06179).
        const bool hasColor = colorFormat != VK_FORMAT_UNDEFINED;
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
        colorBlend.attachmentCount = hasColor ? 1 : 0;
        colorBlend.pAttachments = hasColor ? &blendAttachment : nullptr;

        // --- Dynamic state ---
        VkDynamicState dynamicStates[] = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
        };
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = 3;
        dynamicState.pDynamicStates = dynamicStates;

        // --- Dynamic rendering (Vulkan 1.3) ---
        VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount = hasColor ? 1 : 0;
        renderingInfo.pColorAttachmentFormats = hasColor ? &colorFormat : nullptr;
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
