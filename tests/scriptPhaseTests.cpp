// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
// A script is visited only in the phases it implements. The phases come from the
// script's TYPE at compile time (Script::phasesOf), the registries' factories stamp
// them on each instance, and the script system keeps one component list per phase.
// Nothing on screen depends on any of it — a script visited in a phase it does not
// implement runs an empty method — so these checks are what holds:
//   - phasesOf sees an override made by the type or by a base, and nothing else;
//   - a script from a registry runs exactly as it did when every script was visited:
//     the right methods, in execution order, not while disabled, and a script or a
//     component created mid-phase still runs in that phase;
//   - a component is in a phase's list only when one of its scripts implements the
//     phase, and leaves every list when it is destroyed;
//   - a script made by a hand-written factory, which says nothing about its phases,
//     is visited in all of them.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/script/scriptRegistry.h"
#include "platform/graphics/graphicsDevice.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    // What ran, in order: "<script>:<phase>".
    std::vector<std::string> ran;

    int count(const std::string& what)
    {
        int n = 0;
        for (const auto& entry : ran) {
            n += entry == what ? 1 : 0;
        }
        return n;
    }

    class Idle final : public Script
    {
    public:
        SCRIPT_NAME("idle")
        void initialize() override { ran.push_back("idle:initialize"); }
    };

    class Updater final : public Script
    {
    public:
        SCRIPT_NAME("updater")
        void update(float) override { ran.push_back("updater:update"); }
    };

    class PostOnly final : public Script
    {
    public:
        SCRIPT_NAME("postOnly")
        void postUpdate(float) override { ran.push_back("postOnly:postUpdate"); }
    };

    class FixedOnly final : public Script
    {
    public:
        SCRIPT_NAME("fixedOnly")
        void fixedUpdate(float) override { ran.push_back("fixedOnly:fixedUpdate"); }
    };

    class Everything final : public Script
    {
    public:
        SCRIPT_NAME("everything")
        void update(float) override { ran.push_back("everything:update"); }
        void postUpdate(float) override { ran.push_back("everything:postUpdate"); }
        void fixedUpdate(float) override { ran.push_back("everything:fixedUpdate"); }
    };

    // A base between the script and Script that implements a phase.
    class PostBase : public Script
    {
    public:
        void postUpdate(float) override { ran.push_back("inherited:postUpdate"); }
    };

    class Inherited final : public PostBase
    {
    public:
        SCRIPT_NAME("inherited")
        void update(float) override { ran.push_back("inherited:update"); }
    };

    // An overload beside the override: `&T::update` is ambiguous, which must read as
    // "implemented", never as "skip".
    class Overloaded final : public Script
    {
    public:
        SCRIPT_NAME("overloaded")
        void update(float) override { ran.push_back("overloaded:update"); }
        void update(int) {}
    };

    // Registered through a hand-written factory, so nothing stamps its phases.
    class Custom final : public Script
    {
    public:
        void update(float) override { ran.push_back("custom:update"); }
        void postUpdate(float) override { ran.push_back("custom:postUpdate"); }
    };

    // Named per instance so two of them can report the order they ran in.
    class Ordered final : public Script
    {
    public:
        SCRIPT_NAME("ordered")
        void update(float) override { ran.push_back(label + ":update"); }
        std::string label = "ordered";
    };

    // Creates, mid-update, an Updater on its own component and one on a NEW entity.
    class Spawner final : public Script
    {
    public:
        SCRIPT_NAME("spawner")
        void postUpdate(float) override
        {
            ran.push_back("spawner:postUpdate");
            if (_spawned) {
                return;
            }
            _spawned = true;
            // This component has no script that updates in postUpdate after this one;
            // PostOnly joins it mid-loop.
            entity()->findComponent<ScriptComponent>()->create("postOnly");
            // And a component that was in no list at all joins the one being walked.
            auto owned = std::make_unique<Entity>();
            Entity* other = owned.get();
            other->setEngine(entity()->engine());
            entity()->engine()->root()->addChild(std::move(owned));
            static_cast<ScriptComponent*>(other->addComponent<ScriptComponent>())->create("everything");
        }
    private:
        bool _spawned = false;
    };

    static_assert(Script::phasesOf<Idle>() == 0);
    static_assert(Script::phasesOf<Updater>() == Script::PHASE_UPDATE);
    static_assert(Script::phasesOf<PostOnly>() == Script::PHASE_POST_UPDATE);
    static_assert(Script::phasesOf<FixedOnly>() == Script::PHASE_FIXED_UPDATE);
    static_assert(Script::phasesOf<Everything>() == Script::PHASE_ALL);
    static_assert(Script::phasesOf<Inherited>() == (Script::PHASE_UPDATE | Script::PHASE_POST_UPDATE));
    static_assert(Script::phasesOf<Overloaded>() == Script::PHASE_UPDATE);
    static_assert(Script::phasesOf<Script>() == 0);

    ScriptComponent* addScripts(Engine& engine, Entity*& out)
    {
        auto owned = std::make_unique<Entity>();
        out = owned.get();
        out->setEngine(&engine);
        engine.root()->addChild(std::move(owned));
        return static_cast<ScriptComponent*>(out->addComponent<ScriptComponent>());
    }
}

int main()
{
    auto engine = std::make_shared<Engine>(nullptr);
    AppOptions options;
    options.graphicsDevice = std::make_shared<StubDevice>();
    options.registerComponentSystem<ScriptComponentSystem>();
    engine->init(options);
    engine->scripts()->registerType<Idle>();
    engine->scripts()->registerType<Updater>();
    engine->scripts()->registerType<PostOnly>();
    engine->scripts()->registerType<FixedOnly>();
    engine->scripts()->registerType<Everything>();
    engine->scripts()->registerType<Inherited>();
    engine->scripts()->registerType<Overloaded>();
    engine->scripts()->registerType<Ordered>();
    engine->scripts()->registerType<Spawner>();
    engine->scripts()->registerType("custom", [] { return std::make_unique<Custom>(); });

    auto* system = dynamic_cast<ScriptComponentSystem*>(engine->systems()->getByComponentType<ScriptComponent>());
    check(system != nullptr, "the engine has a script system");
    if (!system) {
        return 1;
    }
    const auto listed = [system](const uint8_t phase) { return system->phaseComponentCount(phase); };

    std::cout << "\nthe phases a registry stamps on an instance\n";
    {
        Entity* entity = nullptr;
        ScriptComponent* scripts = addScripts(*engine, entity);
        check(scripts->create("idle")->phases() == 0, "a script overriding no phase has none");
        check(scripts->create("updater")->phases() == Script::PHASE_UPDATE, "an update-only script has update");
        check(scripts->create("inherited")->phases() == (Script::PHASE_UPDATE | Script::PHASE_POST_UPDATE),
            "a phase implemented by a base counts");
        check(scripts->create("custom")->phases() == Script::PHASE_ALL,
            "a script from a hand-written factory is visited in every phase");
        entity->destroy();
    }
    check(listed(Script::PHASE_UPDATE) == 0 && listed(Script::PHASE_POST_UPDATE) == 0 &&
        listed(Script::PHASE_FIXED_UPDATE) == 0, "destroying the entity takes its component out of every list");

    std::cout << "\na component is listed only in the phases its scripts implement\n";
    Entity* idleEntity = nullptr;
    ScriptComponent* idle = addScripts(*engine, idleEntity);
    ran.clear();
    idle->create("idle");
    check(listed(Script::PHASE_UPDATE) == 0 && listed(Script::PHASE_POST_UPDATE) == 0 &&
        listed(Script::PHASE_FIXED_UPDATE) == 0, "a component of idle scripts is in no phase list");
    check(count("idle:initialize") == 1, "and its script still initializes");

    Entity* mixedEntity = nullptr;
    ScriptComponent* mixed = addScripts(*engine, mixedEntity);
    mixed->create("updater");
    check(listed(Script::PHASE_UPDATE) == 1 && listed(Script::PHASE_POST_UPDATE) == 0,
        "an update-only script lists its component for update alone");
    mixed->create("fixedOnly");
    mixed->create("overloaded");
    check(listed(Script::PHASE_UPDATE) == 1 && listed(Script::PHASE_FIXED_UPDATE) == 1 &&
        listed(Script::PHASE_POST_UPDATE) == 0, "a second script adds its phase, and a shared phase lists once");

    std::cout << "\neach phase runs the scripts that implement it\n";
    ran.clear();
    system->update(0.016f);
    check(ran == std::vector<std::string>{"updater:update", "overloaded:update"}, "update runs the two updaters, in order");
    ran.clear();
    system->postUpdate(0.016f);
    check(ran.empty(), "postUpdate runs nothing");
    ran.clear();
    system->fixedUpdate(0.016f);
    check(ran == std::vector<std::string>{"fixedOnly:fixedUpdate"}, "fixedUpdate runs the one fixed script");

    std::cout << "\ndisabled scripts and entities\n";
    mixed->create("everything", {.enabled = false});
    ran.clear();
    system->update(0.016f);
    system->postUpdate(0.016f);
    check(count("everything:update") == 0 && count("everything:postUpdate") == 0, "a disabled script does not run");
    mixedEntity->setEnabled(false);
    ran.clear();
    system->update(0.016f);
    system->fixedUpdate(0.016f);
    check(ran.empty(), "no script of a disabled entity runs");
    mixedEntity->setEnabled(true);
    ran.clear();
    system->update(0.016f);
    check(count("updater:update") == 1 && count("overloaded:update") == 1, "and they run again once it is enabled");

    std::cout << "\nexecution order holds inside a phase list\n";
    {
        Entity* firstEntity = nullptr;
        Entity* secondEntity = nullptr;
        ScriptComponent* first = addScripts(*engine, firstEntity);
        ScriptComponent* second = addScripts(*engine, secondEntity);
        // Listed in the opposite order to their creation.
        static_cast<Ordered*>(second->create("ordered"))->label = "second";
        static_cast<Ordered*>(first->create("ordered"))->label = "first";
        ran.clear();
        system->update(0.016f);
        const auto position = [](const std::string& what) {
            for (size_t i = 0; i < ran.size(); ++i) {
                if (ran[i] == what) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        check(position("first:update") >= 0 && position("first:update") < position("second:update"),
            "the component created first runs first, whichever got its script first");
        first->setExecutionOrder(second->executionOrder() + 1);
        ran.clear();
        system->update(0.016f);
        check(position("second:update") >= 0 && position("second:update") < position("first:update"),
            "and a changed execution order re-sorts the phase list");
        firstEntity->destroy();
        secondEntity->destroy();
        ran.clear();
        system->update(0.016f);
        check(count("first:update") == 0 && count("second:update") == 0, "destroyed components no longer run");
    }

    std::cout << "\na phase gained while that phase is running\n";
    {
        Entity* entity = nullptr;
        ScriptComponent* scripts = addScripts(*engine, entity);
        scripts->create("spawner");
        const size_t before = listed(Script::PHASE_POST_UPDATE);
        ran.clear();
        system->postUpdate(0.016f);
        check(count("spawner:postUpdate") == 1, "the spawner runs");
        check(count("postOnly:postUpdate") == 1, "a script it adds to its own component runs in the same pass");
        check(count("everything:postUpdate") == 1, "and so does one on a component that joined the list mid-loop");
        check(listed(Script::PHASE_POST_UPDATE) == before + 1, "the new component is listed once");
        ran.clear();
        system->postUpdate(0.016f);
        check(count("spawner:postUpdate") == 1 && count("postOnly:postUpdate") == 1 &&
            count("everything:postUpdate") == 1, "each runs once a pass after that");
    }

    std::cout << "\na script from a hand-written factory\n";
    {
        Entity* entity = nullptr;
        ScriptComponent* scripts = addScripts(*engine, entity);
        scripts->create("custom");
        ran.clear();
        system->update(0.016f);
        system->postUpdate(0.016f);
        check(count("custom:update") == 1 && count("custom:postUpdate") == 1, "runs the phases it implements");
    }

    std::cout << (failures == 0 ? "\nAll script phase tests passed\n" : "\nScript phase tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
