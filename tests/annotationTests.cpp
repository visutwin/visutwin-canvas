// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
// Script lifecycle events (`enable` / `disable` / `state` / `destroy`) and
// the AnnotationManager built on them: registration, hover, the click that shows a tooltip
// and the press elsewhere that hides it, a hotspot behind the camera, an annotation disabled
// or destroyed, and the manager's own teardown.
//
// Input is invisible in a screenshot and the example never clicks, so this is the test that
// says the annotations work. A real engine on a stub device: with no window the canvas is the
// device's 300x150, y down. Under the sanitize preset it is also what catches the manager
// touching an annotation, an entity or a material after it has gone.

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/input/elementInput.h"
#include "framework/script/annotation.h"
#include "framework/script/annotationManager.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/input/mouse.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    /// A script that implements nothing: what the lifecycle events are tested on.
    class Probe : public Script
    {
    public:
        SCRIPT_NAME("probe")
    };

    std::shared_ptr<Engine> engine;
    std::shared_ptr<Mouse> mouse;

    /// Every event `target` fires of `names`, in order.
    std::shared_ptr<std::vector<std::string>> record(EventHandler* target, const std::vector<std::string>& names)
    {
        auto log = std::make_shared<std::vector<std::string>>();
        for (const auto& name : names) {
            target->on(name, [log, name]() { log->push_back(name); });
        }
        return log;
    }

    /// A mouse button event at canvas (x, y), y down, through the engine as an application
    /// feeds it: elements first, then the devices.
    void press(const float x, const float y, const bool down)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.x = x;
        event.button.y = y;
        engine->handleInputEvent(event);
    }

    void move(const float x, const float y)
    {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = x;
        event.motion.y = y;
        engine->handleInputEvent(event);
    }

    void click(const float x, const float y)
    {
        press(x, y, true);
        press(x, y, false);
    }

    /// One frame as far as the annotations see it: update, then prerender. The steps here
    /// reach 0.25 s, past the engine's default 0.1 s clamp; main() raises the clamp.
    void frame(const float dt = 0.0f)
    {
        engine->update(dt);
        engine->fire("prerender");
    }

    bool hasLayer(const std::vector<int>& ids, const std::string& name)
    {
        const auto layer = engine->scene()->layers()->getLayerByName(name);
        return layer && std::find(ids.begin(), ids.end(), layer->id()) != ids.end();
    }
}

REGISTER_SCRIPT(Probe, "probe")

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.size = {300, 150}, .resizable = true});
    auto input = std::make_shared<ElementInput>();
    mouse = std::make_shared<Mouse>();
    engine = makeTestEngine<CameraComponentSystem, RenderComponentSystem, ScreenComponentSystem, ElementComponentSystem,
        ScriptComponentSystem>(device, [&](AppOptions& options) {
        options.elementInput = input;
        options.mouse = mouse;
    });
    engine->setMaxDeltaTime(1.0f);   // the fade cases step by up to 0.25 s

    std::cout << "script lifecycle events\n";
    {
        Entity* parent = newEntity(engine.get(), "parent");
        engine->root()->addChild(std::unique_ptr<GraphNode>(parent));
        Entity* e = newEntity(engine.get(), "scripted");
        e->addComponent<ScriptComponent>();
        Script* probe = e->script()->create<Probe>();
        auto log = record(probe, {"enable", "disable", "destroy"});
        bool lastState = false;
        probe->on("state", [&lastState](const bool state) { lastState = state; });

        parent->addChild(std::unique_ptr<GraphNode>(e));
        check(*log == std::vector<std::string>{"enable"} && lastState,
              "a script created off the scene fires enable (and state true) when its entity joins it");

        log->clear();
        probe->setEnabled(false);
        probe->setEnabled(false);
        check(*log == std::vector<std::string>{"disable"} && !lastState, "setEnabled(false) fires disable once");
        parent->setEnabled(false);
        check(log->size() == 1, "an entity disabled under a disabled script fires nothing");
        probe->setEnabled(true);
        check(log->size() == 1 && !probe->enabled(), "enabling the script under a disabled parent fires nothing");
        parent->setEnabled(true);
        check(*log == std::vector<std::string>{"disable", "enable"} && probe->enabled(),
              "the parent enabled again: enable");

        log->clear();
        e->script()->setEnabled(false);
        check(*log == std::vector<std::string>{"disable"}, "the script component disabled: disable");
        e->script()->setEnabled(true);
        log->clear();

        e->destroy();
        check(*log == std::vector<std::string>{"disable", "destroy"}, "destroying the entity: disable, then destroy");
        (void)e->remove();
        check(*log == std::vector<std::string>{"disable", "destroy"}, "and destroy fires once");
        parent->destroy();
        (void)parent->remove();
    }

    // The camera looks down -Z from z = 10 at the annotation, at the origin; the canvas is
    // 300x150, so the hotspot is at its centre.
    Entity* cameraEntity = newEntity(engine.get(), "camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 10.0f);
    engine->root()->addChild(std::unique_ptr<GraphNode>(cameraEntity));
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    camera->camera()->setAspectRatio(2.0f);

    Entity* owner = newEntity(engine.get(), "owner");
    engine->root()->addChild(std::unique_ptr<GraphNode>(owner));
    owner->addComponent<ScriptComponent>();
    auto* manager = owner->script()->create<AnnotationManager>();

    std::cout << "manager initialization\n";
    check(engine->scene()->layers()->getLayerByName("HotspotBase") != nullptr &&
          engine->scene()->layers()->getLayerByName("HotspotOverlay") != nullptr, "it adds its two layers");
    check(hasLayer(camera->layers(), "HotspotBase") && hasLayer(camera->layers(), "HotspotOverlay") &&
          hasLayer(camera->layers(), "World") && hasLayer(camera->layers(), "UI"),
          "the camera renders them beside its default layers, not instead of them");

    const auto makeAnnotation = [&](const std::string& label, const Vector3& position) {
        Entity* e = newEntity(engine.get(), "annotation" + label);
        e->setLocalPosition(position);
        e->addComponent<ScriptComponent>();
        auto* annotation = e->script()->create<Annotation>();
        annotation->setLabel(label);
        annotation->setTitle("Title " + label);
        annotation->setText("Text " + label);
        owner->addChild(std::unique_ptr<GraphNode>(e));
        return annotation;
    };
    Annotation* first = makeAnnotation("1", Vector3(0.0f, 0.0f, 0.0f));
    auto events = record(first, {"show", "hide"});
    bool hovered = false;
    first->on("hover", [&hovered](const bool hover) { hovered = hover; });
    frame();

    std::cout << "hover and click\n";
    {
        move(150.0f, 75.0f);
        check(hovered && manager->hoverAnnotation() == first, "the mouse over the hotspot hovers it");
        move(10.0f, 10.0f);
        check(!hovered && manager->hoverAnnotation() == nullptr, "and leaving it does not");

        engine->update(0.0f);   // the mouse device's frame ends
        click(150.0f, 75.0f);
        check(manager->activeAnnotation() == first && *events == std::vector<std::string>{"show"},
              "a click on the hotspot shows its tooltip");
        check(!mouse->wasPressed(MouseButton::Left),
              "and the press is withheld from the mouse, so the camera does not orbit with it");

        click(150.0f, 75.0f);
        check(manager->activeAnnotation() == nullptr, "a second click on it hides it");
        frame(0.1f);
        check(events->size() == 1, "hide waits for the 0.2 s fade");
        frame(0.15f);
        check(*events == std::vector<std::string>{"show", "hide"}, "then fires");

        events->clear();
        click(150.0f, 75.0f);
        engine->update(0.0f);
        click(20.0f, 20.0f);
        check(manager->activeAnnotation() == nullptr && mouse->wasPressed(MouseButton::Left),
              "a press anywhere else reaches the mouse and hides the tooltip");
        frame(0.25f);
        check(*events == std::vector<std::string>{"show", "hide"}, "with its hide");
    }

    std::cout << "behind the camera\n";
    {
        cameraEntity->setLocalEulerAngles(0.0f, 180.0f, 0.0f);   // now looking down +Z
        frame();
        click(150.0f, 75.0f);
        check(manager->activeAnnotation() == nullptr, "a hotspot behind the camera cannot be clicked");
        cameraEntity->setLocalEulerAngles(0.0f, 0.0f, 0.0f);
        frame();
        click(150.0f, 75.0f);
        check(manager->activeAnnotation() == first, "and can again once it is in front");
    }

    std::cout << "disable and destroy\n";
    {
        move(150.0f, 75.0f);
        first->entity()->setEnabled(false);
        check(manager->activeAnnotation() == nullptr && manager->hoverAnnotation() == nullptr && !hovered,
              "disabling the annotation's entity drops its tooltip and its hover");
        frame(0.25f);
        click(150.0f, 75.0f);
        check(manager->activeAnnotation() == nullptr, "a disabled annotation cannot be clicked");
        first->entity()->setEnabled(true);
        frame();
        click(150.0f, 75.0f);
        check(manager->activeAnnotation() == first, "enabled again, it can");

        Entity* entity = first->entity();
        entity->destroy();
        (void)entity->remove();
        check(manager->activeAnnotation() == nullptr && manager->hoverAnnotation() == nullptr,
              "destroying the active annotation clears it");
        frame(0.25f);
        click(150.0f, 75.0f);
        check(manager->activeAnnotation() == nullptr, "and its hotspot is gone");

        Annotation* second = makeAnnotation("2", Vector3(0.0f, 0.0f, 0.0f));
        frame();
        click(150.0f, 75.0f);
        check(manager->activeAnnotation() == second, "an annotation added later registers and works");
    }

    std::cout << "manager teardown\n";
    {
        owner->destroy();
        (void)owner->remove();
        check(engine->scene()->layers()->getLayerByName("HotspotBase") == nullptr &&
              engine->scene()->layers()->getLayerByName("HotspotOverlay") == nullptr,
              "destroying the manager removes its layers");
        check(!hasLayer(camera->layers(), "HotspotBase") && hasLayer(camera->layers(), "World"),
              "and takes them off the camera");
        frame(0.25f);
        click(150.0f, 75.0f);
        check(true, "input and frames after it touch nothing it owned");
    }

    engine.reset();
    return finish("annotation");
}
