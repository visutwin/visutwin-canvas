// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanShaderCompiler.h"

#include "spdlog/spdlog.h"

#ifdef VISUTWIN_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif

namespace visutwin::canvas
{
#ifdef VISUTWIN_HAS_SHADERC

    bool vulkanShaderCompilerAvailable()
    {
        return true;
    }

    std::vector<uint32_t> vulkanCompileGlsl(const std::string& source,
        const VulkanShaderStage stage, const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& defines)
    {
        static shaderc::Compiler compiler;

        shaderc::CompileOptions options;
        options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                     shaderc_env_version_vulkan_1_3);
        // Pin SPIR-V 1.3: at 1.6 glslang lowers `discard` to
        // OpDemoteToHelperInvocation, which requires the
        // shaderDemoteToHelperInvocation device feature the engine does not
        // enable (and MoltenVK support varies). 1.3 keeps OpKill.
        options.SetTargetSpirv(shaderc_spirv_version_1_3);
        options.SetOptimizationLevel(shaderc_optimization_level_performance);
        for (const auto& [key, value] : defines) {
            options.AddMacroDefinition(key, value);
        }

        shaderc_shader_kind kind = shaderc_glsl_vertex_shader;
        switch (stage) {
        case VulkanShaderStage::Vertex:   kind = shaderc_glsl_vertex_shader;   break;
        case VulkanShaderStage::Fragment: kind = shaderc_glsl_fragment_shader; break;
        case VulkanShaderStage::Compute:  kind = shaderc_glsl_compute_shader;  break;
        }

        const auto result = compiler.CompileGlslToSpv(source, kind, name.c_str(), options);
        if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
            spdlog::error("Vulkan GLSL compilation failed for '{}':\n{}",
                name, result.GetErrorMessage());
            return {};
        }
        if (result.GetNumWarnings() > 0) {
            spdlog::warn("Vulkan GLSL compilation of '{}' produced {} warning(s):\n{}",
                name, result.GetNumWarnings(), result.GetErrorMessage());
        }

        return {result.cbegin(), result.cend()};
    }

#else // !VISUTWIN_HAS_SHADERC

    bool vulkanShaderCompilerAvailable()
    {
        return false;
    }

    std::vector<uint32_t> vulkanCompileGlsl(const std::string& source,
        const VulkanShaderStage stage, const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& defines)
    {
        (void)source; (void)stage; (void)defines;
        static bool warned = false;
        if (!warned) {
            warned = true;
            spdlog::warn("Vulkan runtime shader compilation requested ('{}') but the "
                         "engine was built without shaderc — falling back to embedded SPIR-V",
                name);
        }
        return {};
    }

#endif // VISUTWIN_HAS_SHADERC
}

#endif // VISUTWIN_HAS_VULKAN
