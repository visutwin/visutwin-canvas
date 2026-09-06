// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Entity lifecycle ordering. All three of these were unheld before 2026-09-06 and
// none of them is visible in a rendered frame, which is why they need a test:
//
//  - enable/disable dispatched in Component::order(), lowest first, disable in
//    reverse. It used to iterate an unordered_map, so a rigid body could be
//    enabled after a component that moves it, in an order that varied per run.
//  - Entity::destroy() tears down descendants first, then disables in order, then
//    releases components in the reverse of creation. The destructor was empty, so
//    components died in container order with no disable pass at all.
//  - onPostStateChange() runs after every component has seen the state change.
//    It was declared and never called.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/entity.h"
#include "framework/components/component.h"
#include "framework/components/componentSystem.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;
    std::vector<std::string> events;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    /// Records what happens to it, so ordering can be asserted rather than eyeballed.
    class ProbeComponent : public Component
    {
    public:
        // No system and no entity: this test exercises Entity's dispatch order, and
        // Component's own base does not touch either during enable/disable.
        ProbeComponent(std::string tag, const int order)
            : Component(nullptr, nullptr), _tag(std::move(tag)), _order(order) {}

        void initializeComponentData() override {}
        [[nodiscard]] int order() const override { return _order; }

        void onEnable() override { events.push_back("enable:" + _tag); }
        void onDisable() override { events.push_back("disable:" + _tag); }
        void onPostStateChange() override { events.push_back("post:" + _tag); }

        ~ProbeComponent() override { events.push_back("destroy:" + _tag); }

    private:
        std::string _tag;
        int _order;
    };

    /// A component that belongs to a real system, so teardown can be checked to go
    /// THROUGH that system rather than around it.
    class OwnedComponent : public Component
    {
    public:
        OwnedComponent(IComponentSystem* system, Entity* entity)
            : Component(system, entity) {}
        void initializeComponentData() override {}
        ~OwnedComponent() override { events.push_back("destroy:owned"); }
    };

    struct EmptyData {};

    class OwnedSystem : public ComponentSystem<OwnedComponent, EmptyData>
    {
    public:
        OwnedSystem() : ComponentSystem<OwnedComponent, EmptyData>(nullptr, "owned") {}
    };

    size_t indexOf(const std::string& what)
    {
        for (size_t i = 0; i < events.size(); ++i) {
            if (events[i] == what) {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    }

    bool before(const std::string& a, const std::string& b)
    {
        const size_t ia = indexOf(a);
        const size_t ib = indexOf(b);
        return ia != static_cast<size_t>(-1) && ib != static_cast<size_t>(-1) && ia < ib;
    }
}

int main()
{
    std::cout << "entity lifecycle ordering\n";

    // "body" carries order -1 like RigidBodyComponent; the other two default to 0
    // and must keep the order they were added in.
    {
        events.clear();
        auto entity = std::make_unique<Entity>();
        // One slot per component: the map holds a single component per type id.
        entity->addComponentInstance(std::make_unique<ProbeComponent>("first", 0), 9001);
        entity->addComponentInstance(std::make_unique<ProbeComponent>("body", -1), 9002);
        entity->addComponentInstance(std::make_unique<ProbeComponent>("second", 0), 9003);

        entity->onHierarchyStateChanged(true);
        check(before("enable:body", "enable:first"), "lower order() is enabled first");
        check(before("enable:first", "enable:second"),
            "equal order keeps creation order");
        check(before("enable:second", "post:body"),
            "onPostStateChange runs only after every component has been enabled");
        check(indexOf("post:first") != static_cast<size_t>(-1),
            "onPostStateChange reaches every component");

        events.clear();
        entity->onHierarchyStateChanged(false);
        check(before("disable:second", "disable:body"),
            "disable runs in REVERSE order, so the body is torn down last");
    }

    // destroy(): descendants before ancestors, and components released in the
    // reverse of the order they were created.
    {
        events.clear();
        auto parent = std::make_unique<Entity>();
        parent->addComponentInstance(std::make_unique<ProbeComponent>("parentA", 0), 9001);
        parent->addComponentInstance(std::make_unique<ProbeComponent>("parentB", 0), 9002);

        auto child = std::make_unique<Entity>();
        child->addComponentInstance(std::make_unique<ProbeComponent>("child", 0), 9001);
        parent->addChild(std::move(child));

        parent->destroy();
        check(before("destroy:child", "destroy:parentA"),
            "a child's components are released before its parent's");
        check(before("destroy:parentB", "destroy:parentA"),
            "components are released in reverse creation order");
        check(parent->destroying(), "destroying() reports the teardown");

        // Idempotent: the destructor calls destroy() too, and nothing may run twice.
        const size_t countBefore = events.size();
        parent->destroy();
        check(events.size() == countBefore, "destroy() is idempotent");
        events.clear();
        parent.reset();
        check(events.empty(), "the destructor re-runs nothing after an explicit destroy");
    }

    // An entity that is never destroyed explicitly still tears down in order.
    {
        events.clear();
        {
            auto entity = std::make_unique<Entity>();
            entity->addComponentInstance(std::make_unique<ProbeComponent>("only", 0), 9001);
        }
        check(indexOf("destroy:only") != static_cast<size_t>(-1),
            "the destructor alone still releases components");
    }

    // Explicit component removal: disabled, announced, then released.
    {
        events.clear();
        auto entity = std::make_unique<Entity>();
        entity->addComponentInstance(std::make_unique<ProbeComponent>("keep", 0), 9001);
        auto* doomed = entity->addComponentInstance(
            std::make_unique<ProbeComponent>("doomed", 0), 9002);

        entity->onHierarchyStateChanged(true);
        events.clear();

        bool announced = false;
        doomed->on("beforeremove", [&announced]() { announced = true; }, nullptr);

        check(entity->removeComponentInstance(9002), "removeComponentInstance reports success");
        check(announced, "beforeremove fires while the component is still valid");
        check(before("disable:doomed", "destroy:doomed"),
            "a removed component is disabled before it is released");
        check(indexOf("destroy:keep") == static_cast<size_t>(-1),
            "removing one component leaves its siblings alone");
        check(!entity->removeComponentInstance(9002),
            "removing an absent component reports false");

        // The survivor must still take part in the lifecycle.
        events.clear();
        entity->onHierarchyStateChanged(false);
        check(indexOf("disable:keep") != static_cast<size_t>(-1),
            "the surviving component is still dispatched to");
    }

    // Teardown must deliver exactly ONE onDisable per component: destroy() sweeps
    // them in order, and the per-component release must not repeat it.
    {
        events.clear();
        auto entity = std::make_unique<Entity>();
        entity->addComponentInstance(std::make_unique<ProbeComponent>("solo", 0), 9001);
        entity->onHierarchyStateChanged(true);
        events.clear();

        entity->destroy();
        int disables = 0;
        for (const auto& e : events) {
            if (e == "disable:solo") {
                ++disables;
            }
        }
        check(disables == 1, "destroy() disables each component exactly once");
        check(before("disable:solo", "destroy:solo"),
            "and disables it before releasing it");
    }

    // Destroying an entity must announce each component through the system that
    // owns it. Teardown used to clear the containers directly, so a system caching
    // its components heard nothing until the component's destructor ran.
    {
        events.clear();
        OwnedSystem system;
        int beforeRemoves = 0;
        int removes = 0;
        system.on("beforeremove", [&beforeRemoves]() { ++beforeRemoves; }, nullptr);
        system.on("remove", [&removes]() { ++removes; }, nullptr);

        auto entity = std::make_unique<Entity>();
        entity->addComponentInstance(
            std::make_unique<OwnedComponent>(&system, entity.get()),
            componentTypeID<OwnedComponent>());

        entity->destroy();
        check(beforeRemoves == 1, "destroy() fires the system's beforeremove");
        check(removes == 1, "destroy() fires the system's remove");
        check(indexOf("destroy:owned") != static_cast<size_t>(-1),
            "and the component is actually released");
    }

    if (failures == 0) {
        std::cout << "entity lifecycle ordering: all checks passed\n";
        return 0;
    }
    std::cout << "entity lifecycle ordering: " << failures << " check(s) FAILED\n";
    return 1;
}
