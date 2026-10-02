// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.07.2026
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanShaderCompiler.h"

#include <atomic>
#include <cstring>

#include "platform/graphics/shaderDiskCache.h"
#include "spdlog/spdlog.h"

#ifdef VISUTWIN_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif

namespace visutwin::canvas
{
    namespace
    {
        std::atomic<const ShaderDiskCache*> spirvDiskCache{nullptr};

        // Everything the SPIR-V is a function of. The first line names the compile
        // settings below (target environment, SPIR-V version, optimisation level):
        // change one and this tag has to change with it, or old entries are served.
        std::string spirvCacheKey(const std::string& source, const VulkanShaderStage stage,
            const std::vector<std::pair<std::string, std::string>>& defines)
        {
            std::string key = "glsl->spirv vulkan1.3 spirv1.3 performance\n";
            key += "stage " + std::to_string(static_cast<int>(stage)) + "\n";
            for (const auto& [name, value] : defines) {
                key += "define " + name + "=" + value + "\n";
            }
            key += source;
            return key;
        }
    }

    void setVulkanShaderDiskCache(const ShaderDiskCache* cache)
    {
        spirvDiskCache.store(cache);
    }

    void releaseVulkanShaderDiskCache(const ShaderDiskCache* cache)
    {
        const ShaderDiskCache* expected = cache;
        spirvDiskCache.compare_exchange_strong(expected, nullptr);
    }

#ifdef VISUTWIN_HAS_SHADERC

    bool vulkanShaderCompilerAvailable()
    {
        return true;
    }

    std::vector<uint32_t> vulkanCompileGlsl(const std::string& source,
        const VulkanShaderStage stage, const std::string& name,
        const std::vector<std::pair<std::string, std::string>>& defines)
    {
        // A previous run's output for exactly this stage, these defines and this
        // source, if there is one.
        const ShaderDiskCache* cache = spirvDiskCache.load();
        std::string cacheKey;
        if (cache && cache->enabled()) {
            cacheKey = spirvCacheKey(source, stage, defines);
            if (const auto stored = cache->load("spirv", cacheKey);
                stored && !stored->empty() && stored->size() % sizeof(uint32_t) == 0) {
                std::vector<uint32_t> words(stored->size() / sizeof(uint32_t));
                std::memcpy(words.data(), stored->data(), stored->size());
                return words;
            }
        }

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

        std::vector<uint32_t> words{result.cbegin(), result.cend()};
        if (cache && cache->enabled() && !words.empty()) {
            cache->store("spirv", cacheKey, {reinterpret_cast<const uint8_t*>(words.data()),
                words.size() * sizeof(uint32_t)});
        }
        return words;
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
