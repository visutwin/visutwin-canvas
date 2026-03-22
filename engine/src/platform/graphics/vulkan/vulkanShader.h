// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan shader implementation — wraps VkShaderModule from SPIR-V bytecode.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vector>
#include <vulkan/vulkan.h>

#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    class VulkanShader : public Shader
    {
    public:
        VulkanShader(GraphicsDevice* device, const ShaderDefinition& definition,
            const std::string& sourceCode = "");

        // Construct from precompiled SPIR-V arrays.
        VulkanShader(GraphicsDevice* device, const ShaderDefinition& definition,
            const uint32_t* vertSpirv, size_t vertWordCount,
            const uint32_t* fragSpirv, size_t fragWordCount);

        ~VulkanShader() override;

        [[nodiscard]] VkShaderModule vertexModule() const { return _vertexModule; }
        [[nodiscard]] VkShaderModule fragmentModule() const { return _fragmentModule; }
        [[nodiscard]] VkShaderModule computeModule() const { return _computeModule; }

    private:
        VkShaderModule createModule(const uint32_t* spirv, size_t wordCount);

        VkDevice _vkDevice = VK_NULL_HANDLE;
        VkShaderModule _vertexModule = VK_NULL_HANDLE;
        VkShaderModule _fragmentModule = VK_NULL_HANDLE;
        VkShaderModule _computeModule = VK_NULL_HANDLE;
    };
}

#endif // VISUTWIN_HAS_VULKAN
