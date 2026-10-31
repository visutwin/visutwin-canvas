// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The Slang compiler behind the engine's single-source shaders. One Slang module is
// compiled to MSL (or a metallib) for Metal, to SPIR-V for Vulkan and to WGSL for a
// WebGPU backend, so a shader is written once and the backends only differ in what
// they are handed. Compiled through the Slang library (vcpkg `shader-slang`) when the
// build has it; otherwise every request fails with a diagnostic saying so.
//
// Variants are specialization constants, declared in the module as
//     [[vk::constant_id(N)]] const uint vtFeatureMaskN = 0;
// which Slang turns into an OpSpecConstant on SPIR-V, a function constant on Metal and
// an `override` on WGSL: the scheme the Vulkan backend already drives, now on both.
// Bindings are declared with BOTH `register(...)`, which decides the Metal slot, and
// `[[vk::binding(binding, set)]]`, which decides the Vulkan and WGSL one; Metal ignores
// the latter and Vulkan the former.
//
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace visutwin::canvas
{
    enum class SlangTarget
    {
        Msl,       ///< Metal Shading Language source, every entry point in one library
        MetalLib,  ///< A compiled Metal library (Slang runs Apple's compiler)
        Spirv,     ///< SPIR-V, one module per entry point
        Wgsl,      ///< WebGPU Shading Language source
    };

    struct SlangCompileRequest
    {
        std::string moduleName;               ///< Names the module in diagnostics
        std::string source;                   ///< The module's Slang source
        std::vector<std::string> entryPoints; ///< `[shader(...)]` functions to compile, in order
        SlangTarget target = SlangTarget::Spirv;
        /// Preprocessor defines. VT_TARGET_METAL is added for the Metal targets.
        std::vector<std::pair<std::string, std::string>> defines;
        std::vector<std::string> searchPaths; ///< Where `import` / `#include` look
        /// Files served from memory by name (the generated bindings.slang, an override of a
        /// module): a lookup by the path's file name wins over the search paths.
        std::vector<std::pair<std::string, std::string>> virtualFiles;
        bool debugInfo = false;
    };

    struct SlangCompileResult
    {
        bool ok = false;
        /// SPIR-V only: one blob per requested entry point, in request order.
        std::vector<std::vector<uint8_t>> entryPointCode;
        /// Msl / MetalLib / Wgsl: the whole program with every entry point.
        std::vector<uint8_t> programCode;
        /// One per requested entry point, in request order: a compute entry's `[numthreads]`
        /// from the linked program's reflection, all zero for another stage.
        std::vector<std::array<uint32_t, 3>> entryPointThreadGroupSizes;
        std::string diagnostics;
        double seconds = 0.0; ///< Wall time of the compile

        [[nodiscard]] std::vector<uint32_t> spirv(size_t entryPoint) const;
        [[nodiscard]] std::string programText() const
        {
            return std::string(programCode.begin(), programCode.end());
        }
    };

    class SlangCompiler
    {
    public:
        /// True when the build links the Slang library.
        [[nodiscard]] static bool available();
        /// The library's version tag, or "unavailable". Goes into cache keys.
        [[nodiscard]] static const char* version();

        /// Compiles a module's entry points for one target. Each call uses a session
        /// of its own, so calls may run on several threads at once.
        [[nodiscard]] static SlangCompileResult compile(const SlangCompileRequest& request);

        /// Declares each of the `readOnly` structured / byte-address buffers `const device` in
        /// Slang's MSL, which emits every one as `device T*`: Apple's compiler warns about a
        /// writable resource in a vertex function, and cannot treat the data as read-only.
        /// Slang suffixes each declaration of `name` (entry parameters, the kernel context's
        /// fields) as `name_N`. compile() applies it to its MSL; the shader bundle's generator
        /// (tools/generate_slang_bundle.py, const_read_only_buffers) does the same: change both.
        [[nodiscard]] static std::string constReadOnlyBuffers(std::string msl, const std::vector<std::string>& readOnly);

        /// Puts each fragment output named `dualSourceSecondary` at colour 0, index 1 in Slang's
        /// MSL, where Metal's dual-source blending reads the second source: Slang emits an
        /// SV_Target1 output as [[color(1)]] whatever its vk::index. compile() applies it; the
        /// bundle's generator (dual_source_outputs) does the same: change both.
        [[nodiscard]] static std::string dualSourceOutputs(std::string msl);
    };
}
