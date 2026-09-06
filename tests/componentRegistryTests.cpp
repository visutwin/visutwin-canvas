// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// ComponentSystemRegistry keeps three containers in step: an owning vector and two
// lookup maps. Before 2026-09-06 `add` wrote the maps unconditionally, so
// registering a second system under an existing id left the FIRST one alive and
// owned — and still subscribed to whatever engine events it had registered for —
// behind an id that no longer resolved to it. That is the same shape as the bug
// upstream's `registry.remove` had, where the list entry outlived the map entry,
// which is why `remove` here erases from all three or from none.

#include <iostream>
#include <memory>
#include <string>

#include "framework/components/componentSystem.h"
#include "framework/components/componentSystemRegistry.h"

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

    /// Distinct component types so two systems can differ by type as well as id.
    class AlphaComponent : public Component
    {
    public:
        AlphaComponent(IComponentSystem* system, Entity* entity) : Component(system, entity) {}
        void initializeComponentData() override {}
    };

    class BetaComponent : public Component
    {
    public:
        BetaComponent(IComponentSystem* system, Entity* entity) : Component(system, entity) {}
        void initializeComponentData() override {}
    };

    struct EmptyData {};

    template <class C>
    class ProbeSystem : public ComponentSystem<C, EmptyData>
    {
    public:
        ProbeSystem(Engine* engine, const std::string& id, bool* aliveFlag)
            : ComponentSystem<C, EmptyData>(engine, id), _alive(aliveFlag)
        {
            if (_alive) { *_alive = true; }
        }
        ~ProbeSystem() override { if (_alive) { *_alive = false; } }

    private:
        bool* _alive;
    };
}

int main()
{
    std::cout << "component system registry\n";

    // A duplicate id is rejected, and the system already wired up survives.
    {
        ComponentSystemRegistry registry;
        bool firstAlive = false;
        bool secondAlive = false;

        auto first = std::make_unique<ProbeSystem<AlphaComponent>>(nullptr, "alpha", &firstAlive);
        auto* firstRaw = first.get();
        registry.add(std::move(first));
        check(registry.getById("alpha") == firstRaw, "the first system is registered");

        // Same id AND same component type — either check must reject it.
        auto second = std::make_unique<ProbeSystem<AlphaComponent>>(nullptr, "alpha", &secondAlive);
        registry.add(std::move(second));

        check(registry.getById("alpha") == firstRaw,
            "a duplicate id does not displace the system already registered");
        check(!secondAlive, "the rejected duplicate is destroyed rather than left owned");
        check(firstAlive, "the original system is still alive");
    }

    // A second system for the same component type under a DIFFERENT id is also
    // rejected: getByComponentType has only one slot, and the loser would otherwise
    // stay alive behind it.
    {
        ComponentSystemRegistry registry;
        bool firstAlive = false;
        bool secondAlive = false;
        registry.add(std::make_unique<ProbeSystem<AlphaComponent>>(nullptr, "alpha", &firstAlive));
        registry.add(std::make_unique<ProbeSystem<AlphaComponent>>(nullptr, "other", &secondAlive));

        check(registry.getById("other") == nullptr,
            "a second system for the same component type is rejected");
        check(!secondAlive, "and is destroyed rather than left owned");
        check(registry.getByComponentType<AlphaComponent>() != nullptr,
            "the type lookup still resolves to the first");
    }

    // remove() clears all three containers together.
    {
        ComponentSystemRegistry registry;
        bool alphaAlive = false;
        bool betaAlive = false;
        auto alpha = std::make_unique<ProbeSystem<AlphaComponent>>(nullptr, "alpha", &alphaAlive);
        auto* alphaRaw = alpha.get();
        registry.add(std::move(alpha));
        registry.add(std::make_unique<ProbeSystem<BetaComponent>>(nullptr, "beta", &betaAlive));

        check(registry.remove(alphaRaw), "remove reports success");
        check(!alphaAlive, "the removed system is destroyed");
        check(registry.getById("alpha") == nullptr, "the id lookup no longer resolves");
        check(registry.getByComponentType<AlphaComponent>() == nullptr,
            "the type lookup no longer resolves");
        check(registry.getById("beta") != nullptr && betaAlive,
            "an unrelated system is untouched");

        // Re-registering the same id must now succeed, which only holds if remove
        // really cleared every container.
        bool replacementAlive = false;
        registry.add(std::make_unique<ProbeSystem<AlphaComponent>>(
            nullptr, "alpha", &replacementAlive));
        check(registry.getById("alpha") != nullptr && replacementAlive,
            "the id can be registered again after removal");
    }

    if (failures == 0) {
        std::cout << "component system registry: all checks passed\n";
        return 0;
    }
    std::cout << "component system registry: " << failures << " check(s) FAILED\n";
    return 1;
}
