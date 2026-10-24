// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Port of upstream gizmos/icon.
//
// Icons mark entities that have nothing else to see or click, such as lights and cameras; click
// one to focus the camera on it. One entity per icon stands at (0, 1, 0), of which the selected
// one is enabled: a camera (rendering nothing), a warm omni light (range 2), a particle system
// (not playing), an anim component and a script component. The IconGizmo is attached to all of
// them, in a layer that keeps the scene depth; the camera starts at 3 x its aspect on each axis
// (zoom 2..20).
//
// Keys stand in for upstream's controls panel:
//   1-5    the entity: light, camera, particle system, anim, script
//   L      draw a light's icon in the light's colour on / off
//   [ / ]  icon size -/+ 4 (12..64)
//   P      projection, perspective / orthographic
// VISUTWIN_GIZMO_ICON=0-4 starts with that entity, for a capture.
//
// DEVIATIONS:
// - No controls panel: keys toggle the settings, and the theme keeps its defaults.
// - There are no audio listener or sound components here, so their two entities are left out.
// - The particle system's component is disabled where upstream's does not auto-play: an icon
//   follows the component's presence, not its state.
// - The camera controls have no `sceneSize` here; with fly off it changes nothing upstream shows.
//
#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>

#include "../overlayGizmoExample.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/gizmo/iconGizmo.h"

using namespace visutwin::canvas;

namespace
{
    // a layer no camera renders, for the subject camera to render nothing
    constexpr int kNoLayer = 9999;
}

class IconGizmoExample final : public OverlayGizmoExample
{
public:
    IconGizmoExample() : OverlayGizmoExample("Icon Gizmo") {}

protected:
    void configure(AppOptions& options) override
    {
        OverlayGizmoExample::configure(options);
        options.registerComponentSystem<ParticleSystemComponentSystem>();
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        if (!createStage(3.0f, Vector2(2.0f, 20.0f), "Overlays")) {
            return false;
        }

        const auto subject = [this](const char* name) {
            auto* entity = new Entity();
            entity->setEngine(engine());
            entity->setName(std::string(name) + " subject");
            root()->addChild(entity);
            entity->setPosition(Vector3(0.0f, 1.0f, 0.0f));
            return entity;
        };

        Entity* light = subject("light");
        auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>());
        lightComponent->setType(LightType::LIGHTTYPE_OMNI);
        lightComponent->setRange(2.0f);
        lightComponent->setColor(Color(1.0f, 0.85f, 0.6f, 1.0f));

        Entity* camera = subject("camera");
        auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
        cameraComponent->setLayers({kNoLayer});
        cameraComponent->camera()->setClearColorBuffer(false);
        cameraComponent->camera()->setClearDepthBuffer(false);
        cameraComponent->setPriority(-1);

        Entity* particles = subject("particlesystem");
        if (auto* component = particles->addComponent<ParticleSystemComponent>()) {
            component->setEnabled(false);
        }

        Entity* anim = subject("anim");
        anim->addComponent<AnimComponent>();

        Entity* script = subject("script");
        script->addComponent<ScriptComponent>();

        _entities = {light, camera, particles, anim, script};

        _gizmo = std::make_unique<IconGizmo>(_camera, _layer);
        watch(_gizmo.get());
        _gizmo->attach(std::vector<GraphNode*>(_entities.begin(), _entities.end()));

        // clicking an icon focuses the camera on its entity
        _gizmo->on(IconGizmo::EVENT_ICONSELECT, [this](GraphNode* node) {
            if (_controls && node) {
                _controls->focus(node->position(), false);
            }
        });

        const char* icon = std::getenv("VISUTWIN_GIZMO_ICON");
        show(icon ? static_cast<size_t>(std::clamp(std::atoi(icon), 0, 4)) : 0);

        spdlog::info("Keys: 1-5 entity | L light colour | [ ] icon size | P projection");
        return true;
    }

    void update(float /*dt*/) override
    {
        handleKeys();
        updateStage({});
    }

    void destroy() override
    {
        _gizmo.reset();
        destroyStage();
    }

private:
    void show(const size_t index)
    {
        for (size_t i = 0; i < _entities.size(); ++i) {
            _entities[i]->setEnabled(i == index);
        }
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard || !_gizmo) {
            return;
        }
        constexpr std::array<Key, 5> digits = {Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4, Key::Digit5};
        for (size_t i = 0; i < digits.size(); ++i) {
            if (keyboard->wasPressed(digits[i])) {
                show(i);
            }
        }
        if (keyboard->wasPressed(Key::L)) _gizmo->useLightColor = !_gizmo->useLightColor;
        if (keyboard->wasPressed(Key::LeftBracket)) _gizmo->setIconSize(std::max(_gizmo->iconSize() - 4.0f, 12.0f));
        if (keyboard->wasPressed(Key::RightBracket)) _gizmo->setIconSize(std::min(_gizmo->iconSize() + 4.0f, 64.0f));
        if (keyboard->wasPressed(Key::P)) toggleProjection();
    }

    std::unique_ptr<IconGizmo> _gizmo;
    std::array<Entity*, 5> _entities{};
};

VISUTWIN_EXAMPLE_MAIN(IconGizmoExample)
