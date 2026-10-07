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

#include "platform/graphics/shaderFeatures.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Shader;
    struct ShaderCode;

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
    };

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
