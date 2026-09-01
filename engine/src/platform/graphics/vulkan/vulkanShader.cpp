// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanShader.h"
#include "vulkanGraphicsDevice.h"

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    VulkanShader::VulkanShader(GraphicsDevice* device, const ShaderDefinition& definition,
        const uint32_t* vertSpirv, size_t vertWordCount,
        const uint32_t* fragSpirv, size_t fragWordCount,
        const uint32_t* instancedVertSpirv, size_t instancedVertWordCount,
        const uint32_t* skyVertSpirv, size_t skyVertWordCount,
        const uint32_t* colorVertSpirv, size_t colorVertWordCount,
        const uint32_t* pointVertSpirv, size_t pointVertWordCount,
        const uint32_t* dynamicBatchVertSpirv, size_t dynamicBatchVertWordCount,
        const uint32_t* skinnedVertSpirv, size_t skinnedVertWordCount,
        const uint32_t* morphedVertSpirv, size_t morphedVertWordCount,
        const uint32_t* skinnedMorphedVertSpirv, size_t skinnedMorphedVertWordCount,
        const bool specializeFeatures,
        const uint32_t* computeSpirv, size_t computeWordCount)
        : Shader(device, definition)
    {
        auto* vkDevice = static_cast<VulkanGraphicsDevice*>(device);
        _vkDevice = vkDevice->device();
        _deviceAlive = vkDevice->aliveToken();
        _features = definition.features;
        _specializeFeatures = specializeFeatures;

        if (vertSpirv && vertWordCount > 0)
            _vertexModule = createModule(vertSpirv, vertWordCount);
        if (fragSpirv && fragWordCount > 0)
            _fragmentModule = createModule(fragSpirv, fragWordCount);
        if (instancedVertSpirv && instancedVertWordCount > 0)
            _instancedVertexModule = createModule(instancedVertSpirv, instancedVertWordCount);
        if (skyVertSpirv && skyVertWordCount > 0)
            _skyVertexModule = createModule(skyVertSpirv, skyVertWordCount);
        if (colorVertSpirv && colorVertWordCount > 0)
            _colorVertexModule = createModule(colorVertSpirv, colorVertWordCount);
        if (pointVertSpirv && pointVertWordCount > 0)
            _pointVertexModule = createModule(pointVertSpirv, pointVertWordCount);
        if (dynamicBatchVertSpirv && dynamicBatchVertWordCount > 0)
            _dynamicBatchVertexModule = createModule(dynamicBatchVertSpirv, dynamicBatchVertWordCount);
        if (skinnedVertSpirv && skinnedVertWordCount > 0)
            _skinnedVertexModule = createModule(skinnedVertSpirv, skinnedVertWordCount);
        if (morphedVertSpirv && morphedVertWordCount > 0)
            _morphedVertexModule = createModule(morphedVertSpirv, morphedVertWordCount);
        if (skinnedMorphedVertSpirv && skinnedMorphedVertWordCount > 0)
            _skinnedMorphedVertexModule = createModule(
                skinnedMorphedVertSpirv, skinnedMorphedVertWordCount);
        if (computeSpirv && computeWordCount > 0)
            _computeModule = createModule(computeSpirv, computeWordCount);

        spdlog::debug("VulkanShader created: {} (vert={} frag={} instanced={} sky={} color={} point={})",
            definition.name,
            _vertexModule != VK_NULL_HANDLE ? "ok" : "none",
            _fragmentModule != VK_NULL_HANDLE ? "ok" : "none",
            _instancedVertexModule != VK_NULL_HANDLE ? "ok" : "none",
            _skyVertexModule != VK_NULL_HANDLE ? "ok" : "none",
            _colorVertexModule != VK_NULL_HANDLE ? "ok" : "none",
            _pointVertexModule != VK_NULL_HANDLE ? "ok" : "none");
    }

    VulkanShader::~VulkanShader()
    {
        // Shaders held by static caches (ProgramLibrary, material device
        // caches) are destroyed after the VkDevice at process exit —
        // vkDestroyShaderModule on a dead device aborts. The device frees its
        // child objects itself, so skipping is correct, not a leak.
        if (_deviceAlive.expired()) {
            return;
        }
        if (_vkDevice != VK_NULL_HANDLE) {
            if (_vertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _vertexModule, nullptr);
            if (_instancedVertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _instancedVertexModule, nullptr);
            if (_skyVertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _skyVertexModule, nullptr);
            if (_colorVertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _colorVertexModule, nullptr);
            if (_pointVertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _pointVertexModule, nullptr);
            if (_dynamicBatchVertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _dynamicBatchVertexModule, nullptr);
            if (_skinnedVertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _skinnedVertexModule, nullptr);
            if (_morphedVertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _morphedVertexModule, nullptr);
            if (_skinnedMorphedVertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _skinnedMorphedVertexModule, nullptr);
            if (_fragmentModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _fragmentModule, nullptr);
            if (_computeModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _computeModule, nullptr);
        }
    }

    VkShaderModule VulkanShader::createModule(const uint32_t* spirv, size_t wordCount)
    {
        VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        createInfo.codeSize = wordCount * sizeof(uint32_t);
        createInfo.pCode = spirv;

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(_vkDevice, &createInfo, nullptr, &module) != VK_SUCCESS) {
            spdlog::error("Failed to create Vulkan shader module");
            return VK_NULL_HANDLE;
        }
        return module;
    }
}

#endif // VISUTWIN_HAS_VULKAN
