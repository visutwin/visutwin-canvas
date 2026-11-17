// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.11.2026
//
// Slang compiles running at once on several threads (Engine::precompile compiles a scene's
// run-time programs side by side). SlangCompiler gives each concurrent compile a global session
// of its own; Slang documents a session as unsafe to share between threads, so a race shows as a
// crash or as code that differs from a serial compile. The first half compiles distinct modules
// serially and then many times at once, for MSL and SPIR-V, and requires byte-identical code.
// The second goes through ProgramLibrary::requestSlangPrograms on the null device: two materials
// with chunk overrides of their own compile the forward program side by side, a second request
// starts nothing, a variant asked for while skipping (setSkipCompilingPrograms) is left unbuilt,
// and what a variant then resolves from is the code a serial compile produces.

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "platform/graphics/null/nullGraphicsDevice.h"
#include "platform/graphics/slangCompiler.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/programLibrary.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace visutwin::canvas
{
    // The library's program lookup and override merge, reached by this test alone.
    struct ProgramLibraryTestAccess
    {
        static const SlangProgramBuild* slangProgram(ProgramLibrary& library, const std::string& program,
            const SlangFileOverrides& overrides)
        {
            return library.slangProgram(program, overrides);
        }
        static SlangFileOverrides chunkOverrides(const ProgramLibrary& library, const Material* material)
        {
            return library.chunkOverrides(material);
        }
    };
}

namespace
{
    // A small module whose output depends on its define, so each request compiles to code of
    // its own.
    constexpr const char* kModule = R"slang(
struct VOut { float4 pos : SV_Position; float3 color : COLOR0; };
[shader("vertex")]
VOut vsMain(uint vid : SV_VulkanVertexID)
{
    VOut o;
    o.pos = float4(float(vid & 1u), float(vid >> 1u), 0.0, 1.0);
    o.color = float3(VALUE * 0.125, 1.0 - VALUE * 0.0625, sin(VALUE));
    return o;
}
[shader("fragment")]
float4 psMain(VOut input) : SV_Target0
{
    float3 c = input.color;
    for (int i = 0; i < VALUE + 2; ++i) { c = c * 0.75 + float3(0.1, 0.2, 0.3) * float(i); }
    return float4(c, 1.0);
}
)slang";

    SlangCompileRequest request(const int value, const SlangTarget target)
    {
        SlangCompileRequest r;
        r.moduleName = "concurrent-" + std::to_string(value);
        r.source = kModule;
        r.entryPoints = {"vsMain", "psMain"};
        r.defines = {{"VALUE", std::to_string(value)}};
        r.target = target;
        return r;
    }

    bool sameCode(const SlangCompileResult& a, const SlangCompileResult& b)
    {
        return a.ok && b.ok && a.programCode == b.programCode && a.entryPointCode == b.entryPointCode;
    }

    // The forward program's user hook scaling the written colour by `scale`, so two materials
    // carrying it with different scales compile two programs.
    std::string userChunk(const std::string& scale)
    {
        return "float3 forwardUserLitColor(float3 color, float3 albedo, float3 worldNormal, float3 worldPos)\n"
            "{ return color; }\n"
            "float forwardUserFogFactor(float fogFactor, float3 worldPos) { return fogFactor; }\n"
            "float3 forwardUserFragmentEnd(float3 color, float3 worldPos, float3 worldNormal)\n"
            "{ return color * " + scale + "; }\n";
    }

    bool sameBuild(const SlangProgramBuild& a, const SlangProgramBuild& b)
    {
        if (a.metalSource != b.metalSource || a.entries.size() != b.entries.size()) {
            return false;
        }
        for (size_t i = 0; i < a.entries.size(); ++i) {
            if (a.entries[i].name != b.entries[i].name || a.entries[i].spirv != b.entries[i].spirv) {
                return false;
            }
        }
        return true;
    }
}

int main()
{
    std::cout << std::unitbuf;
    if (!check(SlangCompiler::available(), "the Slang compiler is available in this build")) {
        return finish("slang-concurrency");
    }

    // 1. The compiler: serial references, then every request at once from several threads.
    constexpr int kModules = 6;
    constexpr int kThreads = 8;
    constexpr int kRounds = 3;
    for (const SlangTarget target : {SlangTarget::Msl, SlangTarget::Spirv}) {
        const std::string name = target == SlangTarget::Msl ? "MSL" : "SPIR-V";
        std::vector<SlangCompileResult> reference;
        for (int value = 0; value < kModules; ++value) {
            reference.push_back(SlangCompiler::compile(request(value, target)));
        }
        bool distinct = true;
        for (int value = 1; value < kModules; ++value) {
            // MSL is one program text, SPIR-V one module per entry point.
            distinct = distinct && (reference[value].programCode != reference[0].programCode ||
                reference[value].entryPointCode != reference[0].entryPointCode);
        }
        check(reference[0].ok && distinct, name + ": the modules compile, each to code of its own");

        std::atomic<int> mismatches{0};
        std::atomic<int> failures{0};
        for (int round = 0; round < kRounds; ++round) {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t] {
                    for (int k = 0; k < kModules; ++k) {
                        const int value = (t + k) % kModules;
                        const SlangCompileResult result = SlangCompiler::compile(request(value, target));
                        if (!result.ok) {
                            failures.fetch_add(1);
                        } else if (!sameCode(result, reference[value])) {
                            mismatches.fetch_add(1);
                        }
                    }
                });
            }
            for (std::thread& thread : threads) {
                thread.join();
            }
        }
        check(failures.load() == 0, name + ": " + std::to_string(kThreads * kModules * kRounds) +
            " compiles at once, none failed (" + std::to_string(failures.load()) + ")");
        check(mismatches.load() == 0, name + ": every concurrent compile is byte-identical to the serial one (" +
            std::to_string(mismatches.load()) + " differ)");
    }

    // 2. The program library: two materials' forward programs compiled side by side.
    auto device = std::make_shared<NullGraphicsDevice>(64, 64);
    auto library = std::make_shared<ProgramLibrary>(device);
    // A is alpha-tested, so its shadow draw runs its opacity frontend under its overrides; B is
    // an opaque caster, whose shadow draws with the shared material-less variants.
    StandardMaterial a;
    StandardMaterial b;
    a.setAlphaMode(AlphaMode::MASK);
    a.setShaderChunk("forward-fragment-user", userChunk("0.5"));
    b.setShaderChunk("forward-fragment-user", userChunk("0.25"));
    StandardMaterial bundled;

    const auto started = std::chrono::steady_clock::now();
    const int requested = library->requestSlangPrograms(&a, true) + library->requestSlangPrograms(&b, true);
    check(requested == 3, "A's forward and shadow programs and B's forward program start, not B's shadow (" +
        std::to_string(requested) + ")");
    check(library->requestSlangPrograms(&a, true) == 0, "asking again starts nothing");
    check(library->requestSlangPrograms(&bundled, true) == 0 || slangRuntimeCompileWanted(),
        "a material without overrides needs no compile (its programs are bundled)");

    // While skipping, a variant of a program still compiling resolves to nothing, and is not
    // remembered: resolved again once skipping is off, it is built (waiting for its program).
    library->setSkipCompilingPrograms(true);
    const auto skippedShader = library->getForwardShader(&b, false, false, false, false, false, false, false, false, 0);
    const int skipped = library->skippedVariants();
    library->setSkipCompilingPrograms(false);
    check((skippedShader == nullptr) == (skipped == 1),
        "skipping: a variant of a program still compiling resolves to null and is counted (" +
        std::to_string(skipped) + " skipped)");
    std::cout << "    B's forward program was " << (skipped ? "still compiling" : "already built") << " when asked\n";

    const SlangFileOverrides overridesA = ProgramLibraryTestAccess::chunkOverrides(*library, &a);
    const SlangFileOverrides overridesB = ProgramLibraryTestAccess::chunkOverrides(*library, &b);
    const SlangProgramBuild* builtA = ProgramLibraryTestAccess::slangProgram(*library, "forward", overridesA);
    const SlangProgramBuild* builtShadowA = ProgramLibraryTestAccess::slangProgram(*library, "shadow", overridesA);
    const SlangProgramBuild* builtB = ProgramLibraryTestAccess::slangProgram(*library, "forward", overridesB);
    const double parallelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    check(builtA && builtShadowA && builtB, "the requested programs are taken over once compiled");
    check(library->pendingSlangPrograms() == 0, "nothing is left compiling");
    check(ProgramLibraryTestAccess::slangProgram(*library, "forward", overridesA) == builtA,
        "a later lookup returns the same build");

    const auto serialStarted = std::chrono::steady_clock::now();
    SlangProgramBuild referenceA;
    SlangProgramBuild referenceB;
    const bool referenced = loadSlangProgram(device.get(), "forward", {}, overridesA, referenceA) &&
        loadSlangProgram(device.get(), "forward", {}, overridesB, referenceB);
    const double serialMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - serialStarted).count();
    check(referenced && builtA && builtB && sameBuild(*builtA, referenceA) && sameBuild(*builtB, referenceB),
        "the programs compiled side by side are the serial compiles' code");
    check(builtA && builtB && !sameBuild(*builtA, *builtB), "the two overrides compiled to two programs");
    std::cout << "    three programs side by side " << parallelMs << " ms, two of them serially " << serialMs << " ms\n";

    check(library->getForwardShader(&a, false, false, false, false, false, false, false, false, 0) != nullptr,
        "a variant resolves from the precompiled program");
    check(library->getForwardShader(&b, false, false, false, false, false, false, false, false, 0) != nullptr,
        "the variant skipped earlier is built once skipping is off");

    library.reset();
    return finish("slang-concurrency");
}
