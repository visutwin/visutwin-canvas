// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A component contributes to a frame only when it is enabled AND its entity is
// enabled in the hierarchy. Component::enabled() answers only the first half, and
// every light-gathering loop tested it alone until 2026-09-11 — so a light on a
// disabled entity, or under a disabled parent, went on lighting the scene while
// mesh instances on the same entity correctly disappeared. These hold the
// predicate those loops now share, and the state it writes into the scene Light.

#include <iostream>
#include <memory>

#include "framework/components/component.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/entity.h"
#include "scene/light.h"

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

    class ProbeComponent : public Component
    {
    public:
        ProbeComponent(IComponentSystem* system, Entity* entity) : Component(system, entity) {}
        void initializeComponentData() override {}
    };

    // A root entity is enabled in the hierarchy the way the scene root is.
    std::unique_ptr<Entity> makeRoot()
    {
        auto root = std::make_unique<Entity>();
        root->setEnabledInHierarchy(true);
        return root;
    }
}

int main()
{
    std::cout << "component active state\n";

    {
        auto root = makeRoot();
        auto* probe = static_cast<ProbeComponent*>(
            root->addComponentInstance(std::make_unique<ProbeComponent>(nullptr, root.get()), 9101));

        check(probe->enabled() && probe->active(),
            "an enabled component on an enabled entity is active");

        probe->setEnabled(false);
        check(!probe->active(), "disabling the component makes it inactive");
        probe->setEnabled(true);

        root->setEnabled(false);
        check(probe->enabled(), "the component's own flag is untouched by the entity");
        check(!probe->active(), "an enabled component on a DISABLED entity is inactive");

        root->setEnabled(true);
        check(probe->active(), "re-enabling the entity makes it active again");
    }

    // The hierarchy half: a parent switched off takes its descendants with it.
    {
        auto root = makeRoot();
        auto childOwned = std::make_unique<Entity>();
        auto* child = childOwned.get();
        root->addChild(std::move(childOwned));

        auto* probe = static_cast<ProbeComponent*>(
            child->addComponentInstance(std::make_unique<ProbeComponent>(nullptr, child), 9101));
        check(probe->active(), "a component under an enabled parent is active");

        root->setEnabled(false);
        check(!probe->active(), "a disabled PARENT makes a descendant's component inactive");
    }

    std::cout << "\nlight component\n";

    // The case the loops got wrong, and the scene Light that three shadow and
    // cookie paths read through.
    {
        auto root = makeRoot();
        auto* light = static_cast<LightComponent*>(
            root->addComponentInstance(
                std::make_unique<LightComponent>(nullptr, root.get()), 9102));
        light->setType(LightType::LIGHTTYPE_DIRECTIONAL);

        check(light->active(), "a light on an enabled entity is active");
        check(light->light() != nullptr && light->light()->enabled(),
            "and its scene Light is enabled");

        root->setEnabled(false);
        check(!light->active(), "a light on a disabled entity is NOT active");
        check(light->enabled(), "though the component's own flag still reads enabled");
        check(!light->light()->enabled(),
            "and the scene Light the shadow and cookie passes read is disabled");

        root->setEnabled(true);
        check(light->active() && light->light()->enabled(),
            "re-enabling the entity brings the light back");

        light->setEnabled(false);
        check(!light->active() && !light->light()->enabled(),
            "disabling the component alone also disables the scene Light");
    }

    std::cout << "\nscript component\n";

    // Scripts run their update phases only while the component is active. They
    // used to gate on the component's own flag, so a script on a disabled entity
    // kept initializing and updating every frame; the phases test active() now.
    {
        auto root = makeRoot();
        auto* scripts = static_cast<ScriptComponent*>(
            root->addComponentInstance(
                std::make_unique<ScriptComponent>(nullptr, root.get()), 9103));

        check(scripts->active(), "a script component on an enabled entity is active");

        root->setEnabled(false);
        check(scripts->enabled(), "the component's own flag is untouched");
        check(!scripts->active(), "a script component on a disabled entity is NOT active");

        root->setEnabled(true);
        check(scripts->active(), "re-enabling the entity makes it active again");
    }

    std::cout << (failures == 0 ? "\nAll component active tests passed\n"
                                : "\nComponent active tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
