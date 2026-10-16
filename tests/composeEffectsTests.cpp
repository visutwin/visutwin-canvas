// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// buildComposeEffects: the compose effects of a camera frame turned into the hook file the
// compose pass compiles and the layout of their uniforms. Which effects are composed (active,
// with a slot and a chunk), the order within a slot (registration), each effect's <ID> and
// defines, one float4 row per uniform and four per matrix with the values packed where the
// accessors read them, an effect left out when the rows run out, the debug views, and a text
// that changes with a define or a uniform's type but not with a uniform's value (a value must
// not compile another shader).
//
// A slot called in the wrong order or a uniform read from the wrong row renders a plausible
// image; the GPU half (tests/slangGpuTests.cpp) draws one composition, this pins the rules.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "scene/graphics/cameraFrameEffect.h"
#include "scene/graphics/composeEffects.h"
#include "scene/graphics/composeShaders.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    std::shared_ptr<CameraFrameEffect> effect(const std::string& id, const ComposeSlot slot, const std::string& slang,
        std::vector<std::string> views = {}, std::vector<FrameResource> resources = {}, std::string entry = {})
    {
        return std::make_shared<CameraFrameEffect>(id, CameraFrameEffectOptions{.slot = slot, .slang = slang,
            .debugViews = std::move(views), .entryPoint = std::move(entry), .requiredResources = std::move(resources)});
    }

    bool contains(const std::string& text, const std::string& part)
    {
        return text.find(part) != std::string::npos;
    }

    // The body of a generated slot function.
    std::string slotBody(const std::string& source, const std::string& slot)
    {
        const auto start = source.find("composeSlot" + slot + "(");
        if (start == std::string::npos) {
            return {};
        }
        const auto end = source.find("return color;", start);
        return source.substr(start, end - start);
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "what is composed\n";
    {
        auto passOnly = std::make_shared<CameraFrameEffect>("passes");
        auto disabled = effect("off", ComposeSlot::Hdr, "float3 applyOff(float3 c, float2 uv) { return c; }");
        disabled->setEnabled(false);
        check(buildComposeEffects({}).source.empty(), "no effect: no generated file (the bundled compose)");
        check(buildComposeEffects({passOnly, disabled}).source.empty(),
            "an effect with no slot, or a disabled one, composes nothing");
        auto noChunk = effect("noChunk", ComposeSlot::Ldr, "");
        const auto build = buildComposeEffects({noChunk});
        check(build.source.empty() && build.errors.size() == 1, "a slot with no chunk is left out with an error");
    }

    std::cout << "debug views of an effect with no slot\n";
    {
        auto viewsOnly = effect("viewer", ComposeSlot::None,
            "float3 debugEdges() { return sceneTexture.SampleLevel(composeDebugUv, 0.0).rgb; }\n",
            {"edges"});
        check(buildComposeEffects({viewsOnly}).source.empty(), "joins nothing while no view is shown");
        check(buildComposeEffects({viewsOnly}, "depth").source.empty(), "nor while another view is");
        const auto build = buildComposeEffects({viewsOnly}, "edges");
        check(build.debugViews == std::vector<std::string>{"edges"}, "joins while one of its views is shown");
        check(composeDebugMode("edges", build) == compose_shaders::kComposeDebugFirstEffect, "as an effect view");
        check(contains(build.source, "static float2 composeDebugUv;") &&
              contains(build.source, "composeEffectDebug(uint view, float2 uv, float3 color)") &&
              contains(build.source, "composeDebugUv = uv;"),
            "the view reads the pixel it is shown for through composeDebugUv");
        check(!contains(slotBody(build.source, "Output"), "applyViewer") &&
              !contains(slotBody(build.source, "Hdr"), "applyViewer"),
            "and is called at no slot");
        viewsOnly->setEnabled(false);
        check(buildComposeEffects({viewsOnly}, "edges").source.empty(), "an inactive one joins nothing");
    }

    std::cout << "identifiers\n";
    {
        check(effect("colorEnhance", ComposeSlot::Hdr, "x")->defineName() == "COLOR_ENHANCE", "colorEnhance -> COLOR_ENHANCE");
        check(effect("tint", ComposeSlot::Hdr, "x")->entryPoint() == "applyTint", "the entry point is apply<Id>");
        check(effect("tint", ComposeSlot::Hdr, "x", {}, {}, "myTint")->entryPoint() == "myTint", "or the one given");
    }

    std::cout << "slots and order\n";
    auto first = effect("first", ComposeSlot::Hdr, "float3 applyFirst(float3 c, float2 uv) { return c; }");
    auto scene = effect("sceneTap", ComposeSlot::Scene, "float4 applySceneTap(float4 c, float2 uv) { return c; }");
    auto second = effect("second", ComposeSlot::Hdr, "float3 applySecond(float3 c, float2 uv) { return c; }",
        {"secondView"}, {FrameResource::Depth});
    auto output = effect("late", ComposeSlot::Output, "float3 applyLate(float3 c, float2 uv) { return c; }",
        {"lateA", "lateB"});
    {
        const auto build = buildComposeEffects({first, scene, second, output});
        const std::string hdr = slotBody(build.source, "Hdr");
        check(build.effects.size() == 4 && build.errors.empty(), "four effects composed");
        check(contains(hdr, "applyFirst(color, uv)") && contains(hdr, "applySecond(color, uv)") &&
              hdr.find("applyFirst") < hdr.find("applySecond"), "a slot calls its effects in registration order");
        check(contains(slotBody(build.source, "Scene"), "applySceneTap") &&
              !contains(slotBody(build.source, "Scene"), "applyFirst"), "each effect only at its own slot");
        check(contains(build.source, "float4 composeSlotScene(float4 color") &&
              contains(build.source, "float3 composeSlotLdr(float3 color"), "every slot is defined, scene on a float4");
        check(contains(build.source, "#define FIRST 1") && contains(build.source, "#define SCENE_TAP 1"),
            "each effect's <ID> is defined");
        check(build.requiresDepth, "an effect requiring the depth makes the composition need it");
        check(build.debugViews == std::vector<std::string>{"secondView", "lateA", "lateB"},
            "the debug views, in effect order");
        check(contains(build.source, "if (view == 2u) {\n        return debugLateB();"),
            "view i calls debug<Name>()");
        using namespace compose_shaders;
        check(composeDebugMode("", build) == kComposeDebugNone && composeDebugMode("bloom", build) == kComposeDebugBloom &&
              composeDebugMode("lateA", build) == kComposeDebugFirstEffect + 1 &&
              composeDebugMode("nobody", build) == kComposeDebugMissing, "debug modes: none, built-in, effect, missing");
    }

    std::cout << "defines and uniforms\n";
    {
        auto tint = effect("tint", ComposeSlot::Ldr, "float3 applyTint(float3 c, float2 uv) { return c; }");
        tint->setUniform("tintStrength", 0.5f);
        tint->setUniform("tintColor", Vector3(1.0f, 2.0f, 3.0f));
        float elements[16];
        for (int i = 0; i < 16; ++i) {
            elements[i] = static_cast<float>(i + 1);
        }
        const Matrix4 matrix = Matrix4::load(elements);
        tint->setUniform("tintMatrix", matrix);
        auto after = effect("after", ComposeSlot::Ldr, "float3 applyAfter(float3 c, float2 uv) { return c; }");
        after->setUniform("afterValue", 7.0f);
        tint->setDefine("TINT_MODE", "2");

        auto build = buildComposeEffects({tint, after});
        const std::string& s = build.source;
        check(contains(s, "#define tintStrength (u.effectData[0].x)"), "a float takes row 0, read as .x");
        check(contains(s, "#define tintColor (u.effectData[1].xyz)"), "a Vector3 the next row, as .xyz");
        check(contains(s, "#define tintMatrix (float4x4(u.effectData[2], u.effectData[3], u.effectData[4], u.effectData[5]))"),
            "a matrix four rows, one per column");
        check(contains(s, "#define afterValue (u.effectData[6].x)"), "the next effect's uniforms follow");
        check(contains(s, "#define TINT_MODE 2"), "a define reaches the chunk");

        float rows[compose_shaders::kComposeEffectDataRows][4] = {};
        packComposeEffectData(build, rows);
        check(rows[0][0] == 0.5f && rows[1][0] == 1.0f && rows[1][2] == 3.0f, "the values packed where they are read");
        check(rows[2][0] == 1.0f && rows[3][0] == 5.0f && rows[5][3] == 16.0f, "the matrix column-major, a column a row");
        check(rows[6][0] == 7.0f, "and the next effect's");

        const std::string before = s;
        tint->setUniform("tintStrength", 0.9f);
        check(buildComposeEffects({tint, after}).source == before, "a new VALUE does not change the shader");
        tint->setUniform("tintStrength", Vector3(0.9f, 0.9f, 0.9f));
        check(buildComposeEffects({tint, after}).source != before, "a new TYPE does");
        tint->setUniform("tintStrength", 0.9f);
        tint->setDefine("TINT_MODE", "3");
        check(buildComposeEffects({tint, after}).source != before, "a changed define does");
        tint->setDefine("TINT_MODE", "");
        check(!contains(buildComposeEffects({tint, after}).source, "TINT_MODE"), "an empty define removes it");
    }

    std::cout << "out of rows\n";
    {
        auto greedy = effect("greedy", ComposeSlot::Hdr, "float3 applyGreedy(float3 c, float2 uv) { return c; }");
        for (int i = 0; i < compose_shaders::kComposeEffectDataRows + 1; ++i) {
            greedy->setUniform("g" + std::to_string(i), 1.0f);
        }
        auto modest = effect("modest", ComposeSlot::Hdr, "float3 applyModest(float3 c, float2 uv) { return c; }");
        modest->setUniform("m", 2.0f);
        const auto build = buildComposeEffects({greedy, modest});
        check(build.effects.size() == 1 && build.effects[0] == modest.get() && build.errors.size() == 1,
            "an effect needing more rows than are left is left out, with an error; the others stay");
        check(contains(build.source, "#define m (u.effectData[0].x)"), "and do not count its rows");
    }

    return finish("compose effects");
}
