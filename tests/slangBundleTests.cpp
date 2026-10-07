// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The Slang program lookup, with no device: the bundle the build compiled holds the pilot
// program with the stages and the targets this build's backends take, and the runtime
// path (what VISUTWIN_SLANG_RUNTIME selects) compiles the same program from the source
// tree for the same backend, serving the generated bindings.slang from the bundle's copy.
// Driven on a stub device, which has a language but no GPU, so it holds the plumbing
// rather than a pixel; tests/slangGpuTests.cpp draws through createShaderFromCode.
#include <cstdint>
#include <iostream>
#include <string>

#include "platform/graphics/shader.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

int main()
{
    SlangBundledProgram upsample;
    if (check(findBundledSlangProgram("upsample", upsample), "the bundle holds the upsample program")) {
        check(upsample.vertexEntry == "upsampleVertex" && upsample.fragmentEntry == "upsampleFragment",
            "with its vertex and fragment entry points");
        check(upsample.computeEntry.empty(), "and no compute stage");
#ifdef VISUTWIN_HAS_METAL
        check(!upsample.metalSource.empty() && upsample.metalSource.find("[[fragment]]") != std::string_view::npos,
            "with MSL holding the fragment stage (Metal build)");
        check(upsample.metalLibrary && upsample.metalLibraryBytes > 4 &&
                upsample.metalLibrary[0] == 'M' && upsample.metalLibrary[1] == 'T' && upsample.metalLibrary[2] == 'L' &&
                upsample.metalLibrary[3] == 'B',
            "and the same compiled to a metallib (Metal build)");
#endif
#ifdef VISUTWIN_HAS_VULKAN
        check(upsample.vertexSpirvWords > 5 && upsample.vertexSpirv[0] == 0x07230203u &&
                upsample.fragmentSpirvWords > 5 && upsample.fragmentSpirv[0] == 0x07230203u,
            "with SPIR-V for both stages (Vulkan build)");
#endif
    }
    SlangBundledProgram missing;
    check(!findBundledSlangProgram("no-such-program", missing), "an unknown program is not in the bundle");

    // The runtime path on a stub device, which reports the Metal language by default.
    {
        StubGraphicsDevice device;
        ShaderCode code;
        const bool compiled = compileSlangProgramFromSource(&device, "upsample", code);
        if (check(compiled, "the upsample program compiles from the source tree for the stub's language")) {
            const bool metal = device.shaderLanguage() == ShaderLanguage::Msl;
            if (metal) {
                check(code.metalSource.find("upsampleFragment") != std::string::npos,
                    "the runtime MSL holds the fragment entry point");
            } else {
                check(!code.vertexSpirv.empty() && !code.fragmentSpirv.empty(), "the runtime SPIR-V holds both stages");
            }
            check(code.specializeFeatures, "the code specializes the feature words");
        }
        ShaderCode none;
        check(!compileSlangProgramFromSource(&device, "no-such-program", none), "a missing program fails");
    }

    return finish("slang bundle");
}
