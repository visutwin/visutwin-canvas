// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// A component type's instance list (ComponentInstanceList), and the per-engine registry
// that keeps one per type (ComponentRegistry, behind Engine::components()).
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
//  - an engine's RenderComponent list, the one the renderer sweeps, behaves the same
//    through real components on real entities.
//
// And the registry's own contract: two engines in one process see only their own
// components; a component built in no engine's hierarchy (as the glTF container builds
// its entities) joins an engine's lists when its entity is inserted under that engine's
// root, at its CREATION position, and moves with its entity to another engine; and a
// component that outlives its engine is let go rather than left pointing into it.

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/components/component.h"
#include "framework/components/componentInstanceList.h"
#include "framework/components/componentRegistry.h"
#include "framework/components/componentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    class Probe final : public Component
    {
    public:
        // No engine, so it joins no registry: listInstance only gives it its creation
        // serial, and the test keeps the list itself.
        explicit Probe(const int id, const bool listNow = true) : Component(nullptr, nullptr), id(id)
        {
            listInstance(this);
            if (listNow) {
                list.add(this);
            }
        }
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

    std::cout << "\nlisted late\n";
    {
        // A component listed after newer ones (its entity reached an engine late) goes
        // where its creation put it, not to the end.
        auto first = std::make_unique<Probe>(1);
        auto late = std::make_unique<Probe>(2, false);
        auto third = std::make_unique<Probe>(3);
        auto fourth = std::make_unique<Probe>(4);
        third.reset();
        Probe::list.add(late.get());
        check(ids() == std::vector<int>({1, 2, 4}), "it is inserted at its creation position");
        late.reset();
        check(ids() == std::vector<int>({1, 4}), "and removed from there");
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
        auto engine = makeTestEngine<RenderComponentSystem>(std::make_shared<StubGraphicsDevice>());
        ComponentRegistry& registry = engine->components();
        std::vector<Entity*> entities;
        std::vector<RenderComponent*> components;
        for (int i = 0; i < 5; ++i) {
            entities.push_back(addTo(engine->root(), newEntity(engine.get())));
            components.push_back(static_cast<RenderComponent*>(entities.back()->addComponent<RenderComponent>()));
        }
        const auto& all = registry.instances<RenderComponent>();
        check(all == components, "five render components join their engine's list, in creation order");

        engine->root()->removeChild(entities[1]).reset();
        engine->root()->removeChild(entities[3]).reset();
        check(registry.instances<RenderComponent>() ==
                std::vector<RenderComponent*>({components[0], components[2], components[4]}),
            "destroying entities removes theirs and keeps the rest in order");
    }

    std::cout << "\ntwo engines\n";
    {
        auto a = makeTestEngine<RenderComponentSystem>(std::make_shared<StubGraphicsDevice>());
        auto b = makeTestEngine<RenderComponentSystem>(std::make_shared<StubGraphicsDevice>());
        auto* onA = static_cast<RenderComponent*>(
            addTo(a->root(), newEntity(a.get()))->addComponent<RenderComponent>());
        auto* onB = static_cast<RenderComponent*>(
            addTo(b->root(), newEntity(b.get()))->addComponent<RenderComponent>());
        check(a->components().instances<RenderComponent>() == std::vector<RenderComponent*>({onA}),
            "an engine lists only its own components");
        check(b->components().instances<RenderComponent>() == std::vector<RenderComponent*>({onB}),
            "and so does the other");
        check(onA->registry() == &a->components() && onB->registry() == &b->components(),
            "each component knows its registry");

        // As the glTF container builds them: no system, an entity in no hierarchy.
        auto* container = new Entity();
        auto* child = new Entity();
        container->addChild(std::unique_ptr<GraphNode>(child));
        auto* early = static_cast<RenderComponent*>(child->addComponentInstance(
            std::make_unique<RenderComponent>(nullptr, child), componentTypeID<RenderComponent>()));
        auto* newer = static_cast<RenderComponent*>(
            addTo(a->root(), newEntity(a.get()))->addComponent<RenderComponent>());
        check(early->registry() == nullptr, "a component built in no engine's hierarchy is in no list");
        check(a->components().instances<RenderComponent>().size() == 2 &&
              b->components().instances<RenderComponent>().size() == 1, "nor in either engine's");

        a->root()->addChild(std::unique_ptr<GraphNode>(container));
        check(early->registry() == &a->components(), "inserting its hierarchy under a root lists it");
        check(a->components().instances<RenderComponent>() ==
                std::vector<RenderComponent*>({onA, early, newer}),
            "at its creation position, before a component made after it");

        b->root()->addChild(container);
        check(early->registry() == &b->components(), "moving its hierarchy to another engine moves it");
        check(a->components().instances<RenderComponent>() == std::vector<RenderComponent*>({onA, newer}) &&
              b->components().instances<RenderComponent>() == std::vector<RenderComponent*>({onB, early}),
            "out of one list and into the other, in creation order");

        // Detached and kept past its engine's life.
        std::unique_ptr<GraphNode> survivor = b->root()->removeChild(container);
        b->destroy();   // out of Engine's process-wide map, so the reset frees it
        b.reset();
        check(early->registry() == nullptr, "a component that outlives its engine is let go");
        survivor.reset();   // its destructor must not reach the freed registry (sanitize)
        check(a->components().instances<RenderComponent>() == std::vector<RenderComponent*>({onA, newer}),
            "and the other engine is untouched");
    }

    return finish("component instance list");
}
