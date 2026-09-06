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

    if (failures == 0) {
        std::cout << "entity lifecycle ordering: all checks passed\n";
        return 0;
    }
    std::cout << "entity lifecycle ordering: " << failures << " check(s) FAILED\n";
    return 1;
}
