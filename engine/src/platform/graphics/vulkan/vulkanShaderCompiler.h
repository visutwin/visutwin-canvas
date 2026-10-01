// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Runtime GLSL -> SPIR-V compilation for the Vulkan backend.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace visutwin::canvas
{
    enum class VulkanShaderStage
    {
        Vertex,
        Fragment,
        Compute,
    };

    /// True when the engine was built with shaderc (VISUTWIN_HAS_SHADERC) and
    /// runtime GLSL compilation is available.
    bool vulkanShaderCompilerAvailable();

    /// Compile GLSL source to SPIR-V for Vulkan 1.3.
    /// `name` labels compile errors; `defines` become preprocessor macros.
    /// Returns an empty vector on failure (error logged) or when the optional
    /// runtime compiler is unavailable. Engine shaders use the build bundle.
    std::vector<uint32_t> vulkanCompileGlsl(const std::string& source,
        VulkanShaderStage stage, const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& defines = {});

    class ShaderDiskCache;
    /// Where vulkanCompileGlsl keeps what it compiles, so the next run reads the
    /// SPIR-V back instead of running the compiler (every quad pass and custom shader
    /// is GLSL compiled at run time; a first frame spends longer in shaderc than in
    /// anything else it does itself). The key is the stage, the defines and the source,
    /// compared in full. Null turns it off. The device that owns the cache sets it and
    /// clears it again before the cache goes away.
    void setVulkanShaderDiskCache(const ShaderDiskCache* cache);
    /// Clears it if it is still `cache` (another device may have set its own since).
    void releaseVulkanShaderDiskCache(const ShaderDiskCache* cache);

}

#endif // VISUTWIN_HAS_VULKAN
