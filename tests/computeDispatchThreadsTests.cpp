// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Compute::setupDispatchThreads and where its workgroup size comes from, without a GPU.
//
// The dispatch is sized from the workgroup size the shader DECLARES: Slang's reflection
// (the bundle's, per compute entry, and a runtime compile's) for Metal, whose kernels declare
// none, and the SPIR-V's LocalSize for Vulkan. A size hard-coded at the call site instead is
// a second copy of the shader's that nothing keeps in step. The dispatch cases are upstream's
// own: whole workgroups for the threads, the thread count and row width the kernel indexes
// with, a one-dimensional dispatch past the per-dimension limit spread over rows.

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "platform/graphics/compute.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/slangCompiler.h"
#include "platform/graphics/spirvReflect.h"
#include "scene/shader-lib/slangShaders.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    /// A stub whose per-dimension workgroup limit a test picks.
    class LimitedDevice final : public StubGraphicsDevice
    {
    public:
        explicit LimitedDevice(const uint32_t maxGroups) { setMaxComputeWorkgroupsPerDimension(maxGroups); }
    };

    std::shared_ptr<Shader> shaderOfSize(GraphicsDevice* device, const std::array<uint32_t, 3>& size)
    {
        ShaderDefinition definition;
        definition.name = "kernel";
        definition.cshader = "main";
        auto shader = std::make_shared<Shader>(device, definition);
        shader->setComputeThreadGroupSize(size);
        return shader;
    }

    std::array<uint32_t, 3> groups(const Compute& compute)
    {
        return {compute.dispatchX(), compute.dispatchY(), compute.dispatchZ()};
    }

    std::string text(const std::array<uint32_t, 3>& v)
    {
        return "[" + std::to_string(v[0]) + ", " + std::to_string(v[1]) + ", " + std::to_string(v[2]) + "]";
    }

    /// A SPIR-V module of a header and the given instructions.
    std::vector<uint32_t> spirv(const std::vector<uint32_t>& instructions)
    {
        std::vector<uint32_t> words = {0x07230203u, 0x00010500u, 0u, 100u, 0u};
        words.insert(words.end(), instructions.begin(), instructions.end());
        return words;
    }

    constexpr const char* kComputeModule = R"(
RWStructuredBuffer<uint> outputs;

[shader("compute")]
[numthreads(8, 4, 2)]
void fill(uint3 id : SV_DispatchThreadID)
{
    outputs[id.x] = id.y;
}

[shader("compute")]
[numthreads(32, 1, 1)]
void other(uint3 id : SV_DispatchThreadID)
{
    outputs[id.x] = 1;
}
)";
}

int main()
{
    std::cout << "threads, in whole workgroups\n";
    {
        LimitedDevice device(65535);
        Compute line(&device, shaderOfSize(&device, {64, 1, 1}));
        line.setupDispatchThreads(100);
        check(groups(line) == std::array<uint32_t, 3>{2, 1, 1}, "100 threads of 64: 2 workgroups, " + text(groups(line)));
        check(line.dispatchThreadCount() == std::array<uint32_t, 3>{100, 1, 1} && line.dispatchRowThreads() == 100,
            "the kernel is told 100 threads, rows of 100");
        line.setupDispatchThreads(128);
        check(groups(line) == std::array<uint32_t, 3>{2, 1, 1}, "128 of 64: exactly 2");

        Compute grid(&device, shaderOfSize(&device, {8, 8, 1}));
        grid.setupDispatchThreads(100, 50);
        check(groups(grid) == std::array<uint32_t, 3>{13, 7, 1}, "100 x 50 threads of 8 x 8: 13 x 7, " + text(groups(grid)));
        check(grid.dispatchThreadCount() == std::array<uint32_t, 3>{100, 50, 1} && grid.dispatchRowThreads() == 100,
            "the thread count and the threads in a row");

        Compute volume(&device, shaderOfSize(&device, {4, 4, 4}));
        volume.setupDispatchThreads(16, 20, 3);
        check(groups(volume) == std::array<uint32_t, 3>{4, 5, 1}, "16 x 20 x 3 threads of 4 x 4 x 4: 4 x 5 x 1");

        Compute none(&device, shaderOfSize(&device, {64, 1, 1}));
        none.setupDispatchThreads(0);
        check(groups(none) == std::array<uint32_t, 3>{1, 1, 1} && none.dispatchThreadCount()[0] == 0,
            "no threads: one workgroup, every thread of it out of range");

        none.setupDispatch(3, 2, 1);
        check(none.dispatchThreadCount() == std::array<uint32_t, 3>{1, 1, 1} && none.dispatchRowThreads() == 1,
            "a plain setupDispatch forgets the thread count");
    }

    std::cout << "\npast the per-dimension limit\n";
    {
        LimitedDevice device(4);
        Compute line(&device, shaderOfSize(&device, {64, 1, 1}));
        line.setupDispatchThreads(64 * 10 + 1);
        check(groups(line) == std::array<uint32_t, 3>{4, 3, 1},
            "641 threads of 64 under a limit of 4: 11 workgroups as 3 rows of 4, " + text(groups(line)));
        check(line.dispatchRowThreads() == 4u * 64u && line.dispatchThreadCount()[0] == 641,
            "rows of 256 threads, still 641 of them");

        Compute fits(&device, shaderOfSize(&device, {64, 1, 1}));
        fits.setupDispatchThreads(4 * 64);
        check(groups(fits) == std::array<uint32_t, 3>{4, 1, 1} && fits.dispatchRowThreads() == 256,
            "exactly at the limit: one row");

        // Only a one-dimensional dispatch over a one-dimensional workgroup spreads; any other
        // is left as it is (and reported).
        Compute grid(&device, shaderOfSize(&device, {8, 8, 1}));
        grid.setupDispatchThreads(8 * 6, 8);
        check(groups(grid) == std::array<uint32_t, 3>{6, 1, 1} && grid.dispatchRowThreads() == 48,
            "a 2D dispatch past the limit is not spread");

        check(Compute::calcDispatchSize(10, 65535) == std::array<uint32_t, 2>{10, 1}, "calcDispatchSize: fits");
        check(Compute::calcDispatchSize(65536, 65535) == std::array<uint32_t, 2>{32768, 2},
            "calcDispatchSize: two rows of half");
        check(Compute::calcDispatchSize(11, 4) == std::array<uint32_t, 2>{4, 3}, "calcDispatchSize: 11 under 4");
    }

    std::cout << "\nthe workgroup size is the shader's\n";
    {
        LimitedDevice device(65535);
        Compute declared(&device, shaderOfSize(&device, {256, 1, 1}));
        check(declared.threadgroupSize() == std::array<uint32_t, 3>{256, 1, 1}, "a declared size is used");
        declared.setThreadgroupSize(64, 1, 1);
        check(declared.threadgroupSize() == std::array<uint32_t, 3>{256, 1, 1},
            "and a disagreeing setThreadgroupSize is ignored");

        Compute undeclared(&device, shaderOfSize(&device, {}));
        check(undeclared.threadgroupSize() == std::array<uint32_t, 3>{8, 8, 1},
            "a shader that declares none (custom MSL): the Compute's, 8 x 8 x 1 by default");
        undeclared.setThreadgroupSize(32, 1, 1);
        undeclared.setupDispatchThreads(100);
        check(groups(undeclared) == std::array<uint32_t, 3>{4, 1, 1}, "or the size set on it");
    }

    std::cout << "\nthe SPIR-V LocalSize\n";
    {
        // OpExecutionMode (16) %1 LocalSize (17) 8 4 2, after an unrelated OpCapability (17).
        const auto words = spirv({(2u << 16) | 17u, 1u, (6u << 16) | 16u, 1u, 17u, 8u, 4u, 2u});
        check(spirvComputeLocalSize(words.data(), words.size()) == std::array<uint32_t, 3>{8, 4, 2},
            "read from its execution mode");
        const auto noMode = spirv({(2u << 16) | 17u, 1u});
        check(spirvComputeLocalSize(noMode.data(), noMode.size()) == std::array<uint32_t, 3>{},
            "none declared: zero");
        const auto truncated = spirv({(6u << 16) | 16u, 1u, 17u});
        check(spirvComputeLocalSize(truncated.data(), truncated.size()) == std::array<uint32_t, 3>{},
            "an instruction past the end: zero");
        const std::vector<uint32_t> garbage = {1u, 2u, 3u, 4u, 5u, 6u};
        check(spirvComputeLocalSize(garbage.data(), garbage.size()) == std::array<uint32_t, 3>{}, "not SPIR-V: zero");
    }

    std::cout << "\nthe bundle carries each compute entry's [numthreads]\n";
    {
        for (const auto& [program, size] : std::vector<std::pair<std::string, std::array<uint32_t, 3>>>{
                 {"instance-cull", {64, 1, 1}}, {"instance-cull-reset", {1, 1, 1}}, {"particle-sim", {256, 1, 1}},
                 {"particle-sort", {256, 1, 1}}}) {
            SlangBundledProgram bundled;
            if (!findBundledSlangProgram(program, bundled)) {
                fail(program + " is in the bundle");
                continue;
            }
            for (const auto& entry : bundled.entries) {
                if (entry.stage != "compute") {
                    continue;
                }
                check(entry.threadGroupSize == size, program + ": " + text(entry.threadGroupSize));
                if (entry.spirv) {
                    check(spirvComputeLocalSize(entry.spirv, entry.spirvWords) == size,
                        program + ": its SPIR-V declares the same");
                }
            }
        }
    }

    std::cout << "\na runtime compile reports it from the reflection\n";
    if (SlangCompiler::available()) {
        for (const SlangTarget target : {SlangTarget::Msl, SlangTarget::Spirv}) {
            SlangCompileRequest request;
            request.moduleName = "dispatch-threads-test";
            request.source = kComputeModule;
            request.entryPoints = {"other", "fill"};
            request.target = target;
            const auto result = SlangCompiler::compile(request);
            const std::string name = target == SlangTarget::Msl ? "MSL" : "SPIR-V";
            check(result.ok, name + ": compiles" + (result.ok ? std::string() : "\n" + result.diagnostics));
            check(result.entryPointThreadGroupSizes.size() == 2 &&
                  result.entryPointThreadGroupSizes[0] == std::array<uint32_t, 3>{32, 1, 1} &&
                  result.entryPointThreadGroupSizes[1] == std::array<uint32_t, 3>{8, 4, 2},
                name + ": one size per requested entry, in request order");
            if (target == SlangTarget::Spirv && result.ok && result.entryPointCode.size() == 2) {
                const auto fill = result.spirv(1);
                check(spirvComputeLocalSize(fill.data(), fill.size()) == std::array<uint32_t, 3>{8, 4, 2},
                    "SPIR-V: and the module agrees");
            }
        }
    } else {
        std::cout << "    skipped: no Slang compiler in this build\n";
    }

    return finish("compute-dispatch-threads");
}
