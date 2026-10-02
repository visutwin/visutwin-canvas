// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// A component type's instance list (ComponentInstanceList, behind T::instances()).
//
// Its ORDER is a contract — creation order is the order draws of equal sort key keep,
// lights take their slots in and scripts run in — and nothing on screen shows a list that
// quietly reordered itself, so it is held here. A removal is a slot lookup that leaves a
// hole, closed in order on the next read, rather than std::erase on a plain vector (a
// scan and a shift per component, so destroying K of N costs K x N). Three things the
// hole scheme must not break:
//
//  - the survivors keep their creation order whatever order the others went in;
//  - a component destroyed WHILE the list is being walked shows as a null entry, never as
//    a dangling pointer and never by shifting a live component past the walker;
//  - RenderComponent::instances(), the list the renderer sweeps, behaves the same through
//    real components on real entities.

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/component.h"
#include "framework/components/componentInstanceList.h"
#include "framework/components/componentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/entity.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class Probe final : public Component
    {
    public:
        explicit Probe(const int id) : Component(nullptr, nullptr), id(id) { list.add(this); }
        ~Probe() override { list.remove(this); }
        void initializeComponentData() override {}

        int id;
        inline static ComponentInstanceList<Probe> list;
    };

    std::vector<int> ids()
    {
        std::vector<int> result;
        for (const Probe* probe : Probe::list.items()) {
            result.push_back(probe ? probe->id : -1);
        }
        return result;
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "order survives removal\n";
    {
        std::vector<std::unique_ptr<Probe>> probes;
        for (int i = 0; i < 8; ++i) {
            probes.push_back(std::make_unique<Probe>(i));
        }
        check(ids() == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7}), "eight components list in creation order");

        // Front, middle and back, in no particular order.
        probes[5].reset();
        probes[0].reset();
        probes[7].reset();
        check(Probe::list.pendingHoles() == 3, "a removal leaves a hole rather than shifting the list");
        check(ids() == std::vector<int>({1, 2, 3, 4, 6}), "the survivors keep their order");
        check(Probe::list.pendingHoles() == 0, "and reading the list closed the holes");

        // A component created after removals goes to the END, not into a hole.
        probes[0] = std::make_unique<Probe>(100);
        check(ids() == std::vector<int>({1, 2, 3, 4, 6, 100}), "a new component is appended");

        // Removal still finds its slot after the list was compacted and appended to.
        probes[3].reset();
        probes[0].reset();
        probes[1].reset();
        check(ids() == std::vector<int>({2, 4, 6}), "removal works after a compaction");

        probes.clear();
        check(ids().empty(), "destroying everything empties the list");
    }

    std::cout << "\nadding while holes are pending\n";
    {
        auto a = std::make_unique<Probe>(1);
        auto b = std::make_unique<Probe>(2);
        auto c = std::make_unique<Probe>(3);
        b.reset();
        auto d = std::make_unique<Probe>(4);   // appended while b's hole is still there
        c.reset();                              // and a removal on either side of it
        check(ids() == std::vector<int>({1, 4}), "holes before and after a late addition close in order");
    }

    std::cout << "\ndestroyed during a walk\n";
    {
        std::vector<std::unique_ptr<Probe>> probes;
        for (int i = 0; i < 6; ++i) {
            probes.push_back(std::make_unique<Probe>(i));
        }

        // The walker destroys the component AFTER the one it is on, then the LAST one.
        // An erase that shifts the rest down makes the walk skip a live component and run
        // one slot past the new end.
        std::vector<int> visited;
        int nulls = 0;
        const auto& items = Probe::list.items();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Probe* probe = items[i];
            if (!probe) {
                ++nulls;
                continue;
            }
            visited.push_back(probe->id);
            if (probe->id == 1) {
                probes[2].reset();
                probes[5].reset();
            }
        }
        check(visited == std::vector<int>({0, 1, 3, 4}), "the walk visits every live component exactly once");
        check(nulls == 2, "and sees the two destroyed ones as null entries");
        check(ids() == std::vector<int>({0, 1, 3, 4}), "the next read has them gone");

        // forEachLive does not compact, so a destructor may use it under someone's walk.
        probes[3].reset();
        std::vector<int> live;
        Probe::list.forEachLive([&live](const Probe* probe) { live.push_back(probe->id); });
        check(live == std::vector<int>({0, 1, 4}), "forEachLive skips holes");
        check(Probe::list.pendingHoles() == 1, "without closing them");
    }

    std::cout << "\nreal components\n";
    {
        const std::size_t before = RenderComponent::instances().size();
        std::vector<std::unique_ptr<Entity>> entities;
        std::vector<RenderComponent*> components;
        for (int i = 0; i < 5; ++i) {
            entities.push_back(std::make_unique<Entity>());
            // No engine, so no system to create it: attached by hand, as a test probe is.
            components.push_back(static_cast<RenderComponent*>(entities.back()->addComponentInstance(
                std::make_unique<RenderComponent>(nullptr, entities.back().get()),
                componentTypeID<RenderComponent>())));
        }
        const auto& all = RenderComponent::instances();
        check(all.size() == before + 5, "five render components join the list");
        bool ordered = true;
        for (std::size_t i = 0; i < 5; ++i) {
            ordered = ordered && all[before + i] == components[i];
        }
        check(ordered, "in creation order");

        entities[1].reset();   // Entity::~Entity destroys its components
        entities[3].reset();
        const auto& after = RenderComponent::instances();
        check(after.size() == before + 3 && after[before] == components[0] &&
              after[before + 1] == components[2] && after[before + 2] == components[4],
            "destroying entities removes theirs and keeps the rest in order");
        entities.clear();
        check(RenderComponent::instances().size() == before, "and the list returns to where it started");
    }

    std::cout << (failures == 0 ? "\nAll component instance list tests passed\n"
        : "\nComponent instance list tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
