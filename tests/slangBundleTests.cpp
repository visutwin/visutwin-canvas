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
//
// In a build with SPIR-V the bundle also carries each entry's reflected layout, and the
// blocks the C++ side fills are held against their structs here: a field added on one side
// only shifts everything after it, which no render shows as an error.
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "extras/script/proceduralSky.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/lightingBlock.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/spirvReflect.h"
#include "scene/graphics/composeShaders.h"
#include "scene/gsplat/gsplatInstance.h"
#include "scene/materials/material.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

#ifdef VISUTWIN_HAS_VULKAN
namespace
{
    // The block a program's entry reflects under `name`, or nullptr.
    const SlangReflectedBinding* reflected(const char* program, const char* entry, const char* name,
        const char* variant)
    {
        static SlangBundledProgram found;
        if (!findBundledSlangProgram(program, found, variant)) {
            return nullptr;
        }
        for (const auto& e : found.entries) {
            if (e.name != entry) {
                continue;
            }
            for (const auto& binding : e.bindings) {
                if (binding.name == name) {
                    return &binding;
                }
            }
        }
        return nullptr;
    }

    void checkBlock(const char* program, const char* entry, const char* name, const uint32_t bytes,
        const char* variant = "")
    {
        const auto* binding = reflected(program, entry, name, variant);
        check(binding && binding->kind == SlangDescriptorKind::UniformBuffer && binding->blockBytes == bytes,
            std::string(program) + (*variant ? std::string("@") + variant : std::string()) + ":" + entry +
            " reflects '" + name + "' as " + std::to_string(bytes) + " bytes" +
            (binding ? " (" + std::to_string(binding->blockBytes) + ")" : " (missing)"));
    }
}
#endif

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

#ifdef VISUTWIN_HAS_VULKAN
    std::cout << "\nreflected blocks against their C++ structs\n";
    checkBlock("forward", "forwardFragment", "lighting", sizeof(LightingBlock));
    checkBlock("forward", "forwardFragment", "material", sizeof(MaterialUniforms));
    checkBlock("forward", "forwardVertex", "material", sizeof(MaterialUniforms));
    checkBlock("shadow", "shadowVsmFragment", "material", sizeof(MaterialUniforms));
    checkBlock("particle-render", "particleVS", "params", sizeof(GpuParticleRenderParams));
    checkBlock("particle-render", "particleFS", "params", sizeof(GpuParticleRenderParams));
    checkBlock("gsplat-render", "gsplatVS", "params", sizeof(GpuGSplatParams), "color");
    checkBlock("gsplat-render", "gsplatVS", "params", sizeof(GpuGSplatParams), "shadow");
    // extras/grid/grid.cpp: GridBlock, asserted 144 bytes there.
    checkBlock("grid", "gridVertex", "grid", 144);
    checkBlock("grid", "gridFragment", "grid", 144);

    // A depth-only shadow pass keeps a fragment stage only when it writes nothing: the splat
    // shadow variant's discard is what shapes the footprint, its moments variant and the
    // colour variant write a target.
    {
        const auto fragmentOf = [](const char* variant) {
            SlangBundledProgram found;
            std::vector<uint32_t> words;
            if (findBundledSlangProgram("gsplat-render", found, variant) && found.fragmentSpirv) {
                words.assign(found.fragmentSpirv, found.fragmentSpirv + found.fragmentSpirvWords);
            }
            return words;
        };
        check(spirvFragmentWritesNoOutput(fragmentOf("shadow")), "the splat shadow fragment writes no output");
        check(!spirvFragmentWritesNoOutput(fragmentOf("shadow-vsm")), "the splat VSM fragment writes its moments");
        check(!spirvFragmentWritesNoOutput(fragmentOf("color")), "the splat colour fragment writes its colour");
        check(!spirvFragmentWritesNoOutput({}), "no code is not a depth-only stage");
    }

    // A Vulkan pipeline declares only the vertex attributes its vertex entry reads. The
    // locations: 0 position, 1 normal, 2 uv0, 3 tangent, 4 uv1.
    {
        const auto inputsOf = [](const char* program, const char* entry) -> std::optional<uint64_t> {
            SlangBundledProgram found;
            if (!findBundledSlangProgram(program, found)) {
                return std::nullopt;
            }
            for (const auto& e : found.entries) {
                if (e.name == entry && e.spirv) {
                    return spirvInputLocations(e.spirv, e.spirvWords);
                }
            }
            return std::nullopt;
        };
        const auto forward = inputsOf("forward", "forwardVertex");
        const auto shadow = inputsOf("shadow", "forwardVertex");
        check(forward && (*forward & 0x1Fu) == 0x1Fu,
            "the forward vertex entry reads position, normal, uv0, tangent and uv1");
        check(shadow && (*shadow & 0x1Fu) == 0x17u,
            "the shadow vertex entry reads position, normal, uv0 and uv1, not the tangent");
        const uint32_t notSpirv[6] = {1, 2, 3, 4, 5, 6};
        check(!spirvInputLocations(notSpirv, 6) && !spirvInputLocations(nullptr, 0),
            "words that are not SPIR-V give no answer");
    }
    checkBlock("procedural-sky", "proceduralSkyFragment", "sky", sizeof(ProceduralSkyBlock));
    checkBlock("procedural-sky-cube", "proceduralSkyCubeFragment", "sky", sizeof(ProceduralSkyBlock));
    checkBlock("compose", "composeFragment", "u", sizeof(compose_shaders::ComposeUniforms));
    SlangBundledProgram forward;
    if (findBundledSlangProgram("forward", forward)) {
        bool all = true;
        for (const auto& entry : forward.entries) {
            if (entry.stage == "vertex" && entry.pushConstantBytes != 128) {
                all = false;
            }
        }
        check(all, "every forward vertex entry reads the 128-byte draw push block");
    }
#endif

    return finish("slang bundle");
}
