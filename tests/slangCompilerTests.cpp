// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The Slang compiler wrapper, with no device: one module compiled to every target the
// backends take. Holds what the single-source scheme relies on, and what the probe
// compiles on the command line established:
//   - a `[[vk::constant_id(N)]]` constant is emitted as a Metal function constant, a
//     SPIR-V specialization constant (OpSpecConstant) and a WGSL `override`;
//   - `register(bN/tN/sN)` decides the Metal slot and `[[vk::binding(b, s)]]` the
//     Vulkan / WGSL one, from one declaration;
//   - the SPIR-V entry point is named `main`, as the Vulkan pipeline binds it;
//   - a broken module fails with a diagnostic, not a crash, and the entry points of a
//     good one come back in request order.
// Compile times are printed for the record (phase 0 of the migration measures them).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "platform/graphics/slangCompiler.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr const char* kModule = R"(
struct Params { float4x4 model; float4 tint; };
[[vk::binding(0, 0)]] ConstantBuffer<Params> params : register(b3);
[[vk::binding(34, 1)]] Texture2D opacityMap : register(t34);
[[vk::binding(24, 1)]] SamplerState materialSampler : register(s24);
[[vk::constant_id(0)]] const uint vtFeatureMask0 = 0;

struct VSIn { [[vk::location(0)]] float3 position : POSITION; [[vk::location(2)]] float2 uv : TEXCOORD0; };
struct VSOut { float4 clip : SV_Position; float2 uv : TEXCOORD0; };

[shader("vertex")]
VSOut vsMain(VSIn i)
{
    VSOut o;
    o.clip = mul(float4(i.position, 1.0), params.model);
    o.uv = i.uv;
    return o;
}

[shader("fragment")]
float4 psMain(VSOut i) : SV_Target0
{
    float a = opacityMap.Sample(materialSampler, i.uv).r;
    float3 c = (vtFeatureMask0 & 1u) != 0u ? params.tint.rgb : float3(a, a, a);
    return float4(c, 1.0);
}
)";

    bool contains(const std::string& text, const char* needle)
    {
        return text.find(needle) != std::string::npos;
    }

    // Finds `OpSpecConstant` (opcode 50, word count 4) anywhere in the module.
    bool hasSpecConstant(const std::vector<uint32_t>& words)
    {
        for (size_t i = 5; i < words.size();) {
            const uint32_t count = words[i] >> 16u;
            const uint32_t opcode = words[i] & 0xffffu;
            if (count == 0) {
                return false;
            }
            if (opcode == 50) {
                return true;
            }
            i += count;
        }
        return false;
    }

    // The entry point's name: OpEntryPoint (15) carries the literal string from word 3.
    std::string entryPointName(const std::vector<uint32_t>& words)
    {
        for (size_t i = 5; i < words.size();) {
            const uint32_t count = words[i] >> 16u;
            const uint32_t opcode = words[i] & 0xffffu;
            if (count == 0) {
                break;
            }
            if (opcode == 15 && count > 3) {
                const char* text = reinterpret_cast<const char*>(&words[i + 3]);
                return std::string(text, strnlen(text, (count - 3) * sizeof(uint32_t)));
            }
            i += count;
        }
        return {};
    }

    SlangCompileRequest request(const SlangTarget target)
    {
        SlangCompileRequest r;
        r.moduleName = "slang-compiler-test";
        r.source = kModule;
        r.entryPoints = {"vsMain", "psMain"};
        r.target = target;
        return r;
    }
}

int main()
{
    std::cout << "Slang " << SlangCompiler::version() << '\n';
    if (!check(SlangCompiler::available(), "the Slang compiler is available in this build")) {
        return finish("slang compiler");
    }

    // SPIR-V: one module per entry point, named main, with the spec constant.
    {
        const auto result = SlangCompiler::compile(request(SlangTarget::Spirv));
        std::cout << "  spirv compile: " << result.seconds * 1000.0 << " ms\n";
        if (check(result.ok, "the module compiles to SPIR-V: " + result.diagnostics)) {
            check(result.entryPointCode.size() == 2, "two SPIR-V modules, one per entry point");
            for (size_t i = 0; i < result.entryPointCode.size(); ++i) {
                const auto words = result.spirv(i);
                check(!words.empty() && words[0] == 0x07230203u, "entry point " + std::to_string(i) + " is SPIR-V");
                check(entryPointName(words) == "main", "entry point " + std::to_string(i) + " is named main");
            }
            check(hasSpecConstant(result.spirv(1)), "the fragment module carries an OpSpecConstant");
        }
        // Warm: the second compile of the same module, for the cache-less cost.
        const auto again = SlangCompiler::compile(request(SlangTarget::Spirv));
        std::cout << "  spirv compile (again): " << again.seconds * 1000.0 << " ms\n";
    }

    // MSL: one library with both entry points, the register() slots and a function constant.
    {
        const auto result = SlangCompiler::compile(request(SlangTarget::Msl));
        std::cout << "  msl compile: " << result.seconds * 1000.0 << " ms\n";
        if (check(result.ok, "the module compiles to MSL: " + result.diagnostics)) {
            const std::string msl = result.programText();
            check(contains(msl, "[[vertex]]") && contains(msl, "[[fragment]]"), "the MSL holds both stages");
            check(contains(msl, "[[function_constant(0)]]"), "the feature word is a function constant");
            check(contains(msl, "[[buffer(3)]]"), "register(b3) is Metal buffer 3");
            check(contains(msl, "[[texture(34)]]"), "register(t34) is Metal texture 34");
            check(contains(msl, "[[sampler(24)]]"), "register(s24) is Metal sampler 24");
            check(contains(msl, "[[attribute(0)]]") && contains(msl, "[[attribute(2)]]") &&
                    !contains(msl, "[[attribute(1)]]"),
                "a vertex input's vk::location is its Metal attribute index too");
        }
    }

    // WGSL: the WebGPU backend's input, with an override and the Vulkan-style bindings.
    {
        const auto result = SlangCompiler::compile(request(SlangTarget::Wgsl));
        std::cout << "  wgsl compile: " << result.seconds * 1000.0 << " ms\n";
        if (check(result.ok, "the module compiles to WGSL: " + result.diagnostics)) {
            const std::string wgsl = result.programText();
            check(contains(wgsl, "@id(0) override"), "the feature word is a WGSL override");
            check(contains(wgsl, "@binding(34) @group(1)"), "vk::binding(34, 1) is WGSL binding 34 of group 1");
            check(contains(wgsl, "@binding(0) @group(0)"), "vk::binding(0, 0) is WGSL binding 0 of group 0");
        }
    }

    // A define reaches the preprocessor.
    {
        auto r = request(SlangTarget::Msl);
        r.source = "#ifndef VT_PROBE\n#error VT_PROBE is not defined\n#endif\n" + r.source;
        r.defines = {{"VT_PROBE", "1"}};
        const auto result = SlangCompiler::compile(r);
        check(result.ok, "a request's defines reach the module: " + result.diagnostics);
    }

    // Failure is a diagnostic.
    {
        auto r = request(SlangTarget::Spirv);
        r.source = "[shader(\"fragment\")] float4 psMain() : SV_Target0 { return undefinedThing; }";
        r.entryPoints = {"psMain"};
        const auto result = SlangCompiler::compile(r);
        check(!result.ok, "a module with an undefined identifier fails");
        check(contains(result.diagnostics, "undefinedThing"), "and the diagnostic names it");
        auto missing = request(SlangTarget::Spirv);
        missing.entryPoints = {"vsMain", "noSuchEntry"};
        const auto missingResult = SlangCompiler::compile(missing);
        check(!missingResult.ok && contains(missingResult.diagnostics, "noSuchEntry"),
            "a missing entry point fails and is named");
    }

    // Read-only buffers are `const device` in the MSL: slangc emits every structured buffer as
    // `device T*`, which Apple's compiler warns about in a vertex function. A RW buffer, and a
    // local pointer copied from it, stay writable.
    {
        std::cout << "\nread-only buffers in MSL\n";
        SlangCompileRequest r;
        r.moduleName = "buffers";
        r.source = R"slang(
[[vk::binding(0, 0)]] StructuredBuffer<float4> data : register(t3);
[[vk::binding(1, 0)]] ByteAddressBuffer raw : register(t5);
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> written : register(u4);
struct VOut { float4 pos : SV_Position; };
[shader("vertex")]
VOut bufferVertex(uint vid : SV_VulkanVertexID) { VOut o; o.pos = data[vid] + raw.Load<float4>(vid * 16); return o; }
[shader("fragment")]
float4 bufferFragment(float4 pos : SV_Position) : SV_Target0 { written[uint(pos.x)] = data[0]; return data[1]; }
)slang";
        r.entryPoints = {"bufferVertex", "bufferFragment"};
        r.target = SlangTarget::Msl;
        const auto result = SlangCompiler::compile(r);
        if (check(result.ok, "the module compiles to MSL: " + result.diagnostics)) {
            const std::string msl = result.programText();
            check(contains(msl, "const device* data_") && contains(msl, "const device* raw_"),
                "a StructuredBuffer and a ByteAddressBuffer are const device");
            check(!contains(msl, "const device* written_") && contains(msl, "device* written_"),
                "a RWStructuredBuffer stays writable");
        }

        const std::string sample =
            "struct K { float4 device* data_0; float4 device* out_0; };\n"
            "vertex V vs(float4 device* data_1 [[buffer(3)]], float4 device* out_1 [[buffer(4)]]) {\n"
            "    float4 device* _S1 = context->data_0+i;\n"
            "    float4 device* _S2 = (&context_1)->data_0+i;\n"
            "    float4 device* _S3 = out_1+i;\n"
            "    float4 device* data_10x = nullptr;\n"
            "}\n";
        const std::string done = SlangCompiler::constReadOnlyBuffers(sample, {"data"});
        check(contains(done, "float4 const device* data_0;") && contains(done, "float4 const device* data_1 [[buffer(3)]]"),
            "the kernel context field and the entry parameter of a read-only buffer are const");
        check(contains(done, "const device* _S1 = context->data_0") &&
                  contains(done, "const device* _S2 = (&context_1)->data_0"),
            "so are the local pointers copied from it");
        check(contains(done, "float4 device* out_0;") && contains(done, "float4 device* _S3 = out_1"),
            "another buffer, and pointers copied from it, are left alone");
        check(contains(done, "float4 device* data_10x"), "a longer name that only starts the same is left alone");
    }

    // A dual-source output is at colour 0, index 1 in the MSL: Slang emits it as colour 1.
    {
        std::cout << "\nthe dual-source output on Metal\n";
        const std::string sample =
            "struct ForwardDualSourceOut_0 {\n"
            "    float4 color_0 [[color(0)]];\n"
            "    float4 dualSourceSecondary_0 [[color(1)]];\n"
            "};\n"
            "struct ForwardMrt2Out_0 { float4 color0_0 [[color(0)]]; float4 color1_0 [[color(1)]]; };\n";
        const std::string done = SlangCompiler::dualSourceOutputs(sample);
        check(contains(done, "float4 dualSourceSecondary_0 [[color(0), index(1)]];"),
            "the second source goes to colour 0, index 1");
        check(contains(done, "float4 color_0 [[color(0)]];") && contains(done, "float4 color1_0 [[color(1)]];"),
            "another output, colour 1 of a second attachment included, is left alone");
    }

    // VISUTWIN_SLANG_TIME_MODULE=<file.slang> [VISUTWIN_SLANG_TIME_ENTRY=<name>]: compile that
    // module in-process to SPIR-V and MSL twice each and print the times, for sizing the
    // front end on a real module without the CLI's process start and core-module load.
    if (const char* path = std::getenv("VISUTWIN_SLANG_TIME_MODULE")) {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        const char* entry = std::getenv("VISUTWIN_SLANG_TIME_ENTRY");
        SlangCompileRequest r;
        r.moduleName = "timed";
        r.source = text.str();
        r.entryPoints = {entry ? entry : "psMain"};
        for (const auto target : {SlangTarget::Spirv, SlangTarget::Msl}) {
            r.target = target;
            for (int k = 0; k < 2; ++k) {
                const auto result = SlangCompiler::compile(r);
                std::cout << "  timed " << path << " -> " << (target == SlangTarget::Spirv ? "SPIR-V" : "MSL")
                          << ": " << result.seconds * 1000.0 << " ms" << (result.ok ? "" : " FAILED: " + result.diagnostics)
                          << '\n';
            }
        }
    }

    return finish("slang compiler");
}
