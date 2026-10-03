// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// What Engine::update does with a frame, before any system sees it:
//   - the order of the update phases: the systems' update, the scripts' update, then
//     animation, then postUpdate (systems, then scripts). A script that sets an animation
//     parameter in update() must be read by the SAME frame's animation; with the scripts
//     after animation it applies one frame late, which no single screenshot shows;
//   - the time step: clamped to maxDeltaTime (0.1 s by default) and then multiplied by
//     timeScale, once, on the path applications use (they call update(dt) themselves).
//     Every phase, the "update" event and the fixed-update accumulator get that step.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/script/scriptRegistry.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-6f;

    std::vector<std::string> ran;
    std::vector<float> steps;

    class OrderProbe final : public Script
    {
    public:
        SCRIPT_NAME("orderProbe")
        void update(const float dt) override
        {
            ran.emplace_back("script:update");
            steps.push_back(dt);
        }
        void postUpdate(float) override { ran.emplace_back("script:postUpdate"); }
    };

    /// Every step the frame's phases received, all equal to `expected`.
    bool everyStepIs(const float expected)
    {
        if (steps.empty()) {
            return false;
        }
        for (const float step : steps) {
            if (!near(step, expected, kTolerance)) {
                return false;
            }
        }
        return true;
    }
}

int main()
{
    auto engine = makeTestEngine<ScriptComponentSystem>(std::make_shared<StubGraphicsDevice>());
    engine->scripts()->registerType<OrderProbe>();

    engine->systems()->on("update", [](const float dt) {
        ran.emplace_back("systems:update");
        steps.push_back(dt);
    });
    engine->systems()->on("animationUpdate", [](const float dt) {
        ran.emplace_back("systems:animationUpdate");
        steps.push_back(dt);
    });
    engine->systems()->on("postUpdate", [](const float dt) {
        ran.emplace_back("systems:postUpdate");
        steps.push_back(dt);
    });
    engine->on("update", [](const float dt) {
        ran.emplace_back("engine:update");
        steps.push_back(dt);
    });
    int fixedSteps = 0;
    engine->systems()->on("fixedUpdate", [&fixedSteps](float) { ++fixedSteps; });

    auto owned = std::make_unique<Entity>();
    Entity* entity = owned.get();
    entity->setEngine(engine.get());
    engine->root()->addChild(std::move(owned));
    static_cast<ScriptComponent*>(entity->addComponent<ScriptComponent>())->create<OrderProbe>();

    std::cout << "the order of the update phases\n";
    {
        ran.clear();
        steps.clear();
        engine->update(0.016f);
        check(ran == std::vector<std::string>{"systems:update", "script:update", "systems:animationUpdate",
                                              "systems:postUpdate", "script:postUpdate", "engine:update"},
            "systems update, scripts update, animation, systems postUpdate, scripts postUpdate, then the event");
    }

    std::cout << "\nthe time step\n";
    {
        check(near(engine->maxDeltaTime(), 0.1f, kTolerance) && near(engine->timeScale(), 1.0f, kTolerance),
            "the defaults are a 0.1 s clamp and a time scale of 1");

        steps.clear();
        engine->update(0.05f);
        check(everyStepIs(0.05f), "a step under the clamp passes through unchanged");

        steps.clear();
        engine->update(1.0f);
        check(steps.size() == 5 && everyStepIs(0.1f), "update(1.0) reaches every phase and the event as 0.1");

        steps.clear();
        engine->setTimeScale(0.5f);
        engine->update(1.0f);
        check(everyStepIs(0.05f), "with a time scale of 0.5 it reaches them as 0.05: clamped, then scaled");

        steps.clear();
        engine->update(0.04f);
        check(everyStepIs(0.02f), "the scale applies under the clamp too");

        steps.clear();
        engine->setTimeScale(1.0f);
        engine->update(-1.0f);
        check(everyStepIs(0.0f), "a negative step is clamped to zero");

        steps.clear();
        engine->setMaxDeltaTime(2.0f);
        engine->update(2.0f);
        check(near(engine->maxDeltaTime(), 2.0f, kTolerance) && everyStepIs(2.0f),
            "a raised clamp lets a deliberately large step through");
        engine->setMaxDeltaTime(0.1f);

        // The accumulator sees the clamped step: a 10 s stall is one clamp's worth of
        // fixed updates, not a capped run of catch-up substeps. Binary-exact values (a
        // 0.125 s clamp, 1/32 s substeps) make that exactly four, whatever is left in the
        // accumulator from before (always less than one substep).
        engine->setMaxDeltaTime(0.125f);
        engine->setFixedDeltaTime(1.0f / 32.0f);
        engine->setMaxFixedSubSteps(100);
        engine->update(0.0f);
        fixedSteps = 0;
        engine->update(10.0f);
        check(fixedSteps == 4, "the fixed-update accumulator receives the clamped step");
        engine->setMaxDeltaTime(0.1f);

        engine->setTimeScale(0.0f);
        steps.clear();
        fixedSteps = 0;
        engine->update(0.05f);
        check(everyStepIs(0.0f) && fixedSteps == 0, "a time scale of 0 pauses everything that advances with time");
        engine->setTimeScale(1.0f);
    }

    return finish("engine update");
}
