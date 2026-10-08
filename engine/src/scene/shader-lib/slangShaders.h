// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The single-source shader programs: one Slang file per program under
// engine/shaders/slang/programs, compiled for every backend by the build into
// slang_shader_bundle.h, and created on a device through GraphicsDevice::createShaderFromCode.
//
// A program's code comes from the bundle unless VISUTWIN_SLANG_RUNTIME is set in the
// environment (or the bundle has no such program): then the source tree is found through
// shaderSourceRoots(), the program and its modules are compiled through SlangCompiler for
// the device's backend, with the generated bindings.slang served from the bundle's copy
// and the SPIR-V kept in the device's persistent shader cache. That is the hot-reload and
// override path; the bundle is what a shipped build uses.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/graphics/shaderFeatures.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Shader;
    struct ShaderCode;

    /// A descriptor's kind, as an entry's reflected layout names it.
    enum class SlangDescriptorKind : uint8_t
    {
        UniformBuffer,
        StorageBuffer,
        CombinedImageSampler,
        SampledImage,
        Sampler
    };

    /// One descriptor an entry point's SPIR-V uses, from the build's reflection.
    struct SlangReflectedBinding
    {
        std::string_view name;
        uint32_t set = 0;
        uint32_t binding = 0;
        SlangDescriptorKind kind = SlangDescriptorKind::UniformBuffer;
        uint32_t blockBytes = 0;   // a uniform block's size; 0 otherwise
    };

    /// A program the build compiled into the bundle, as the backends consume it.
    struct SlangBundledProgram
    {
        std::string_view name;
        std::string_view vertexEntry;    // empty when the program has no such stage
        std::string_view fragmentEntry;
        std::string_view computeEntry;
        std::string_view metalSource;    // MSL with every entry point; empty when Metal was not built
        const uint8_t* metalLibrary = nullptr;  // the same as a compiled metallib; null when not built
        size_t metalLibraryBytes = 0;
        const uint32_t* vertexSpirv = nullptr;
        size_t vertexSpirvWords = 0;
        const uint32_t* fragmentSpirv = nullptr;
        size_t fragmentSpirvWords = 0;
        const uint32_t* computeSpirv = nullptr;
        size_t computeSpirvWords = 0;

        /// Every entry point, in source order (a program composed from chunks may have
        /// several per stage), with its own SPIR-V when Vulkan was built.
        struct Entry
        {
            std::string_view stage;   // "vertex", "fragment" or "compute"
            std::string_view name;
            const uint32_t* spirv = nullptr;
            size_t spirvWords = 0;
            /// The descriptors this entry's SPIR-V uses and its push-constant size: the
            /// layout the Vulkan backend checks its descriptor contract against. Empty when
            /// the build compiled no SPIR-V.
            std::vector<SlangReflectedBinding> bindings;
            uint32_t pushConstantBytes = 0;
        };
        std::vector<Entry> entries;
    };

    /// A program's code for one device, every entry point included: what loadSlangProgram
    /// hands a caller that assembles its own shader (the forward program picks its vertex
    /// entry per variant).
    struct SlangProgramBuild
    {
        struct Entry
        {
            std::string stage;
            std::string name;
            std::vector<uint32_t> spirv;   // Vulkan only
        };
        std::string metalSource;           // Metal: MSL with every entry point
        std::vector<uint8_t> metalLibrary; // Metal: the same, compiled (the bundle's)
        std::vector<Entry> entries;
    };

    /// File overrides for a runtime compile: a file NAME (`forward-fragment-lights.slang`) and
    /// the source that replaces that file of the tree wherever it is included.
    using SlangFileOverrides = std::vector<std::pair<std::string, std::string>>;

    /// Every entry point of `program` / `variant` for `device`: from the bundle, or compiled
    /// from the source tree when VISUTWIN_SLANG_RUNTIME is set, the bundle lacks the program,
    /// or `overrides` is not empty (a chunk override). False, with the diagnostics logged,
    /// when neither works.
    [[nodiscard]] bool loadSlangProgram(GraphicsDevice* device, std::string_view program, std::string_view variant,
        const SlangFileOverrides& overrides, SlangProgramBuild& out);

    /// The bundle's entry for `program` / `variant`, or nothing. A program declares its
    /// variants with `// @variant <name>: DEFINE=value ...` lines (each compiled with those
    /// defines); one without such lines has the single variant "".
    [[nodiscard]] bool findBundledSlangProgram(std::string_view program, SlangBundledProgram& out,
        std::string_view variant = {});

    /// Compiles `program` / `variant` from the source tree for `device`'s backend, as the
    /// override path does: the code is filled in and true returned, or the diagnostics are
    /// logged. Public so a test can drive the runtime path without the environment variable.
    [[nodiscard]] bool compileSlangProgramFromSource(GraphicsDevice* device, std::string_view program,
        ShaderCode& code, std::string_view variant = {});

    /// The shader for `program` / `variant` on `device`, cached on the device under
    /// "slang:<program>[@<variant>]" (with the feature set's hash when `features` is not
    /// empty): from the bundle, or from the source tree when VISUTWIN_SLANG_RUNTIME is set
    /// or the bundle lacks it. Null, with an error logged, when neither is possible.
    std::shared_ptr<Shader> getOrCreateSlangShader(GraphicsDevice* device, const std::string& program,
        const std::string& variant = {}, const ShaderFeatureSet& features = {});
}
