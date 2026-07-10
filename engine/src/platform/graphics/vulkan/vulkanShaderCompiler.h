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
    /// Returns an empty vector on failure (error logged) or when the compiler
    /// is unavailable — callers fall back to embedded precompiled SPIR-V.
    std::vector<uint32_t> vulkanCompileGlsl(const std::string& source,
        VulkanShaderStage stage, const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& defines = {});

    /// Read a GLSL source file from engine/shaders/vulkan (path resolved from
    /// the source tree or the working directory, like the Metal chunk
    /// registry). Returns empty when not found.
    std::string vulkanReadShaderSourceFile(const std::string& filename);
}

#endif // VISUTWIN_HAS_VULKAN
