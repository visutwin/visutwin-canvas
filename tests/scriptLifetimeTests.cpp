// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A script may, from inside its own update, create another script on the same
// component or destroy its own entity. Until 2026-09-24 the component's loops
// range-iterated its script vector, so the first reallocated the vector under the
// loop and the second freed the component — and the running script — mid-loop:
// both undefined behaviour that usually still "worked". The sanitizer build (CI's
// macos-sanitize job) turns either into a hard failure; the checks here pin what
// must happen instead:
//   - a script created mid-update runs in the same pass, and the scripts after it
//     are still updated;
//   - after a script destroys its entity, no later script on it runs, and the
//     script that did it can still touch its own members until it returns.

#include <iostream>
#include <memory>
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

    // Update counts per script type, in the order scripts ran.
    std::vector<std::string> ran;

    class Spawner final : public Script
    {
    public:
        SCRIPT_NAME("spawner")
        void update(float) override
        {
            ran.push_back("spawner");
            if (!_spawned) {
                _spawned = true;
                // The component holds two scripts with room for two: this third one
                // reallocates the vector the component is iterating.
                entity()->findComponent<ScriptComponent>()->create("late");
            }
        }
    private:
        bool _spawned = false;
    };

    class Counter final : public Script
    {
    public:
        SCRIPT_NAME("counter")
        void update(float) override { ran.push_back("counter"); }
    };

    class Late final : public Script
    {
    public:
        SCRIPT_NAME("late")
        void update(float) override { ran.push_back("late"); }
    };

    class Destroyer final : public Script
    {
    public:
        SCRIPT_NAME("destroyer")
        void update(float) override
        {
            ran.push_back("destroyer");
            entity()->destroy();
            // Still inside this script's method, with the component gone: its own
            // members must still be alive until it returns.
            _afterDestroy = 42;
            ran.push_back(_afterDestroy == 42 ? "destroyer-returned" : "destroyer-corrupt");
        }
    private:
        int _afterDestroy = 0;
    };

    class After final : public Script
    {
    public:
        SCRIPT_NAME("after")
        void update(float) override { ran.push_back("after"); }
    };

    int count(const char* name)
    {
        int n = 0;
        for (const auto& entry : ran) {
            n += entry == name ? 1 : 0;
        }
        return n;
    }

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
    engine->scripts()->registerType<Spawner>();
    engine->scripts()->registerType<Counter>();
    engine->scripts()->registerType<Late>();
    engine->scripts()->registerType<Destroyer>();
    engine->scripts()->registerType<After>();

    std::cout << "a script creates another mid-update\n";
    {
        Entity* entity = nullptr;
        ScriptComponent* scripts = addScripts(*engine, entity);
        check(scripts != nullptr, "the entity has a script component");
        scripts->create("spawner");
        scripts->create("counter");
        ran.clear();
        scripts->updateScripts(0.016f);
        check(count("spawner") == 1 && count("counter") == 1,
            "the scripts after the spawner still run after the vector reallocates");
        check(count("late") == 1, "the new script runs in the same pass");
        scripts->updateScripts(0.016f);
        check(count("spawner") == 2 && count("counter") == 2 && count("late") == 2,
            "and all three run every pass after that");
    }

    std::cout << "\na script destroys its own entity mid-update\n";
    {
        Entity* entity = nullptr;
        ScriptComponent* scripts = addScripts(*engine, entity);
        scripts->create("destroyer");
        scripts->create("after");
        ran.clear();
        scripts->updateScripts(0.016f);   // `scripts` is freed inside this call
        check(count("destroyer") == 1 && count("destroyer-returned") == 1,
            "the destroying script finishes with its own state intact");
        check(count("destroyer-corrupt") == 0, "nothing it owned was freed under it");
        check(count("after") == 0, "no later script on the destroyed entity runs");
        check(entity->findComponent<ScriptComponent>() == nullptr, "the component is gone");
    }

    std::cout << (failures == 0 ? "\nAll script lifetime tests passed\n" : "\nScript lifetime tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
