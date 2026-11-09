// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// CameraControls' mouse buttons and ground pan, driven through the engine's real Mouse
// as an application feeds it: the default mapping (left orbits, middle pans, right looks),
// remapped buttons driving the actions they were given, a button set to NONE leaving the
// camera alone, and the ground pan keeping the camera's height while holding the point of
// the ground under the cursor.
//
// A real engine on a stub device: with no window the canvas is the device's 300x150, y down.
// Every scenario builds its own camera rig and destroys it afterwards, so one rig's mouse
// subscriptions never move another.

#include <iostream>
#include <memory>

#include <SDL3/SDL.h>

#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "extras/script/cameraControls.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/input/mouse.h"
#include "scene/camera.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    using Action = CameraControls::MouseAction;

    std::shared_ptr<Engine> engine;

    // Where every rig starts: looking down at the origin from above and behind.
    const Vector3 kStart(0.0f, 6.0f, 12.0f);

    struct Rig
    {
        Entity* entity = nullptr;
        CameraComponent* camera = nullptr;
        CameraControls* controls = nullptr;
    };

    void frame()
    {
        engine->update(0.1f);
    }

    /// Several frames: the orbit controller's pan damping (0.9, not reachable from the
    /// script) leaves a few hundred-thousandths of a pan after one 0.1 s frame.
    void settle()
    {
        for (int i = 0; i < 4; ++i) {
            frame();
        }
    }

    Rig makeRig()
    {
        Rig rig;
        rig.entity = newEntity(engine.get(), "camera");
        rig.entity->setLocalPosition(kStart.getX(), kStart.getY(), kStart.getZ());
        engine->root()->addChild(std::unique_ptr<GraphNode>(rig.entity));
        rig.camera = static_cast<CameraComponent*>(rig.entity->addComponent<CameraComponent>());
        rig.camera->camera()->setAspectRatio(2.0f);
        rig.entity->addComponent<ScriptComponent>();
        rig.controls = rig.entity->script()->create<CameraControls>();
        rig.controls->setRotateDamping(0.0f);
        rig.controls->setZoomDamping(0.0f);
        // The first update sets the controls up and subscribes them to the mouse.
        frame();
        return rig;
    }

    void destroyRig(const Rig& rig)
    {
        rig.entity->destroy();
        [[maybe_unused]] auto freed = engine->root()->removeChild(rig.entity);
    }

    void button(const Uint8 sdlButton, const float x, const float y, const bool down)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.button = sdlButton;
        event.button.down = down;
        event.button.x = x;
        event.button.y = y;
        engine->handleInputEvent(event);
    }

    void moveTo(const float x, const float y, const float dx, const float dy)
    {
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = x;
        event.motion.y = y;
        event.motion.xrel = dx;
        event.motion.yrel = dy;
        engine->handleInputEvent(event);
    }

    /// A drag of `sdlButton` from (x, y) by (dx, dy): press, an update, the movement, an
    /// update, the release, then the controllers settle.
    void drag(const Uint8 sdlButton, const float x, const float y, const float dx, const float dy)
    {
        button(sdlButton, x, y, true);
        frame();
        moveTo(x + dx, y + dy, dx, dy);
        frame();
        button(sdlButton, x + dx, y + dy, false);
        settle();
    }

    Vector3 forwardOf(Entity* entity)
    {
        return entity->rotation() * Vector3(0.0f, 0.0f, -1.0f);
    }

    /// The point of the horizontal plane at `height` under canvas (x, y).
    Vector3 groundUnder(const CameraComponent* camera, const float x, const float y, const float height)
    {
        const Vector3 nearPoint = camera->screenToWorld(x, y, camera->camera()->nearClip());
        const Vector3 farPoint = camera->screenToWorld(x, y, camera->camera()->farClip());
        const float t = (height - nearPoint.getY()) / (farPoint.getY() - nearPoint.getY());
        return Vector3::lerp(nearPoint, farPoint, t);
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{.size = {300, 150}});
    auto mouse = std::make_shared<Mouse>();
    engine = makeTestEngine<CameraComponentSystem, ScriptComponentSystem>(device, [&](AppOptions& options) {
        options.mouse = mouse;
    });

    constexpr float eps = 1e-3f;

    std::cout << "default mapping\n";
    {
        const Rig rig = makeRig();
        CameraControls* controls = rig.controls;
        check(controls->leftButton() == Action::ORBIT, "left button orbits by default");
        check(controls->middleButton() == Action::PAN, "middle button pans by default");
        check(controls->rightButton() == Action::LOOK, "right button looks by default");
        check(!controls->groundPan(), "ground pan is off by default");

        const Vector3 focus0 = controls->focusPoint();
        const float distance0 = (rig.entity->position() - focus0).length();
        const Vector3 forward0 = forwardOf(rig.entity);

        drag(SDL_BUTTON_LEFT, 150.0f, 75.0f, 20.0f, 0.0f);
        check(controls->mode() == CameraControls::Mode::ORBIT, "a left drag orbits");
        check(near(controls->focusPoint(), focus0, eps), "an orbit keeps the focus point");
        check(near((rig.entity->position() - controls->focusPoint()).length(), distance0, eps),
            "an orbit keeps the distance");
        check(!near(forwardOf(rig.entity), forward0, eps), "an orbit turns the view");

        const Vector3 position1 = rig.entity->position();
        const Vector3 focus1 = controls->focusPoint();
        const Vector3 forward1 = forwardOf(rig.entity);
        drag(SDL_BUTTON_MIDDLE, 150.0f, 75.0f, 20.0f, 15.0f);
        check(controls->mode() == CameraControls::Mode::ORBIT, "a middle drag stays in orbit");
        check(!near(controls->focusPoint(), focus1, eps), "a middle drag moves the focus point");
        check(near(forwardOf(rig.entity), forward1, eps), "a middle drag does not turn the view");
        check(!near(rig.entity->position().getY(), position1.getY(), eps),
            "a vertical pan across the plane of a tilted view changes the camera's height");

        const Vector3 forward2 = forwardOf(rig.entity);
        drag(SDL_BUTTON_RIGHT, 150.0f, 75.0f, 20.0f, 0.0f);
        check(controls->mode() == CameraControls::Mode::FLY, "a right drag switches to fly");
        check(!near(forwardOf(rig.entity), forward2, eps), "a right drag turns the view");

        destroyRig(rig);
    }

    std::cout << "remapped buttons\n";
    {
        const Rig rig = makeRig();
        CameraControls* controls = rig.controls;
        controls->setLeftButton(Action::PAN);
        controls->setMiddleButton(Action::LOOK);
        controls->setRightButton(Action::NONE);
        check(controls->leftButton() == Action::PAN && controls->middleButton() == Action::LOOK &&
              controls->rightButton() == Action::NONE, "the getters answer the actions set");

        const Vector3 focus0 = controls->focusPoint();
        const Vector3 forward0 = forwardOf(rig.entity);
        drag(SDL_BUTTON_LEFT, 150.0f, 75.0f, 20.0f, 10.0f);
        check(controls->mode() == CameraControls::Mode::ORBIT, "a left drag mapped to PAN stays in orbit");
        check(!near(controls->focusPoint(), focus0, eps), "a left drag mapped to PAN moves the focus point");
        check(near(forwardOf(rig.entity), forward0, eps), "a left drag mapped to PAN does not turn the view");

        const Vector3 position1 = rig.entity->position();
        const Vector3 forward1 = forwardOf(rig.entity);
        drag(SDL_BUTTON_RIGHT, 150.0f, 75.0f, 40.0f, 25.0f);
        check(controls->mode() == CameraControls::Mode::ORBIT, "a right press mapped to NONE changes no mode");
        check(near(rig.entity->position(), position1, eps) && near(forwardOf(rig.entity), forward1, eps),
            "a right drag mapped to NONE leaves the camera where it was");

        drag(SDL_BUTTON_MIDDLE, 150.0f, 75.0f, 20.0f, 0.0f);
        check(controls->mode() == CameraControls::Mode::FLY, "a middle press mapped to LOOK switches to fly");
        check(!near(forwardOf(rig.entity), forward1, eps), "a middle drag mapped to LOOK turns the view");

        destroyRig(rig);
    }

    std::cout << "ground pan\n";
    {
        const Rig rig = makeRig();
        CameraControls* controls = rig.controls;
        controls->setGroundPan(true);
        check(controls->groundPan(), "ground pan reads back on");

        const float height = controls->focusPoint().getY();
        const Vector3 position0 = rig.entity->position();
        const Vector3 forward0 = forwardOf(rig.entity);
        const Vector3 grabbed = groundUnder(rig.camera, 150.0f, 75.0f, height);

        drag(SDL_BUTTON_MIDDLE, 150.0f, 75.0f, 30.0f, 20.0f);
        check(near(rig.entity->position().getY(), position0.getY(), eps), "a ground pan keeps the camera's height");
        check(near(controls->focusPoint().getY(), height, eps), "a ground pan keeps the focus point's height");
        check((rig.entity->position() - position0).length() > 0.1f, "a ground pan moves the camera");
        check(near(forwardOf(rig.entity), forward0, eps), "a ground pan does not turn the view");
        check(near(groundUnder(rig.camera, 180.0f, 95.0f, height), grabbed, 1e-2f),
            "the ground point grabbed stays under the cursor");

        // Any button mapped to PAN pans across the ground likewise.
        const Vector3 position1 = rig.entity->position();
        controls->setLeftButton(Action::PAN);
        drag(SDL_BUTTON_LEFT, 150.0f, 75.0f, -25.0f, 30.0f);
        check(near(rig.entity->position().getY(), position1.getY(), eps),
            "a ground pan on a remapped button keeps the camera's height");

        destroyRig(rig);
    }

    engine.reset();
    return finish("camera-controls");
}
