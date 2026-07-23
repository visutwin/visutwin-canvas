// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan shader implementation — wraps VkShaderModule from SPIR-V bytecode.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    class VulkanShader : public Shader
    {
    public:
        // Construct from precompiled SPIR-V arrays.  The instanced and skybox
        // vertex stages are optional — when present, the pipeline selects them
        // for instanced draws / skybox materials respectively.
        VulkanShader(GraphicsDevice* device, const ShaderDefinition& definition,
            const uint32_t* vertSpirv, size_t vertWordCount,
            const uint32_t* fragSpirv, size_t fragWordCount,
            const uint32_t* instancedVertSpirv = nullptr, size_t instancedVertWordCount = 0,
            const uint32_t* skyVertSpirv = nullptr, size_t skyVertWordCount = 0,
            const uint32_t* colorVertSpirv = nullptr, size_t colorVertWordCount = 0,
            const uint32_t* pointVertSpirv = nullptr, size_t pointVertWordCount = 0,
            const uint32_t* dynamicBatchVertSpirv = nullptr, size_t dynamicBatchVertWordCount = 0,
            const uint32_t* skinnedVertSpirv = nullptr, size_t skinnedVertWordCount = 0,
            const uint32_t* morphedVertSpirv = nullptr, size_t morphedVertWordCount = 0,
            const uint32_t* skinnedMorphedVertSpirv = nullptr,
            size_t skinnedMorphedVertWordCount = 0,
            bool specializeFeatures = false,
            const uint32_t* computeSpirv = nullptr, size_t computeWordCount = 0);

        ~VulkanShader() override;

        [[nodiscard]] VkShaderModule vertexModule() const { return _vertexModule; }
        [[nodiscard]] VkShaderModule instancedVertexModule() const { return _instancedVertexModule; }
        [[nodiscard]] VkShaderModule skyVertexModule() const { return _skyVertexModule; }
        // 72-byte vertex-color layout variant (attribute 5 = vec4 color @56).
        [[nodiscard]] VkShaderModule colorVertexModule() const { return _colorVertexModule; }
        // 28-byte point-cloud layout variant (pos @0 + vec4 color @12, unlit).
        [[nodiscard]] VkShaderModule pointVertexModule() const { return _pointVertexModule; }
        [[nodiscard]] VkShaderModule dynamicBatchVertexModule() const { return _dynamicBatchVertexModule; }
        [[nodiscard]] VkShaderModule skinnedVertexModule() const { return _skinnedVertexModule; }
        [[nodiscard]] VkShaderModule morphedVertexModule() const { return _morphedVertexModule; }
        [[nodiscard]] VkShaderModule skinnedMorphedVertexModule() const { return _skinnedMorphedVertexModule; }
        [[nodiscard]] VkShaderModule fragmentModule() const { return _fragmentModule; }
        [[nodiscard]] VkShaderModule computeModule() const { return _computeModule; }
        [[nodiscard]] uint64_t featureMask() const { return _featureMask; }
        [[nodiscard]] bool specializesFeatures() const {
            return _specializeFeatures;
        }

    private:
        VkShaderModule createModule(const uint32_t* spirv, size_t wordCount);

        VkDevice _vkDevice = VK_NULL_HANDLE;
        // Expired => the device (and all its child objects) is already gone;
        // the destructor must not call vkDestroyShaderModule on it.
        std::weak_ptr<bool> _deviceAlive;
        VkShaderModule _vertexModule = VK_NULL_HANDLE;
        VkShaderModule _instancedVertexModule = VK_NULL_HANDLE;
        VkShaderModule _skyVertexModule = VK_NULL_HANDLE;
        VkShaderModule _colorVertexModule = VK_NULL_HANDLE;
        VkShaderModule _pointVertexModule = VK_NULL_HANDLE;
        VkShaderModule _dynamicBatchVertexModule = VK_NULL_HANDLE;
        VkShaderModule _skinnedVertexModule = VK_NULL_HANDLE;
        VkShaderModule _morphedVertexModule = VK_NULL_HANDLE;
        VkShaderModule _skinnedMorphedVertexModule = VK_NULL_HANDLE;
        VkShaderModule _fragmentModule = VK_NULL_HANDLE;
        VkShaderModule _computeModule = VK_NULL_HANDLE;
        uint64_t _featureMask = 0;
        bool _specializeFeatures = false;
    };
}

#endif // VISUTWIN_HAS_VULKAN
