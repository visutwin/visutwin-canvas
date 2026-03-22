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
        const std::string& sourceCode)
        : Shader(device, definition)
    {
        auto* vkDevice = static_cast<VulkanGraphicsDevice*>(device);
        _vkDevice = vkDevice->device();
        (void)sourceCode;
        // Runtime GLSL compilation not supported — use the SPIR-V constructor.
    }

    VulkanShader::VulkanShader(GraphicsDevice* device, const ShaderDefinition& definition,
        const uint32_t* vertSpirv, size_t vertWordCount,
        const uint32_t* fragSpirv, size_t fragWordCount)
        : Shader(device, definition)
    {
        auto* vkDevice = static_cast<VulkanGraphicsDevice*>(device);
        _vkDevice = vkDevice->device();

        if (vertSpirv && vertWordCount > 0)
            _vertexModule = createModule(vertSpirv, vertWordCount);
        if (fragSpirv && fragWordCount > 0)
            _fragmentModule = createModule(fragSpirv, fragWordCount);

        spdlog::debug("VulkanShader created: {} (vert={} frag={})",
            definition.name,
            _vertexModule != VK_NULL_HANDLE ? "ok" : "none",
            _fragmentModule != VK_NULL_HANDLE ? "ok" : "none");
    }

    VulkanShader::~VulkanShader()
    {
        if (_vkDevice != VK_NULL_HANDLE) {
            if (_vertexModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(_vkDevice, _vertexModule, nullptr);
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
