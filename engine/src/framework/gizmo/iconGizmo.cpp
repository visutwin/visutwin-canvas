// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
#include "iconGizmo.h"

#include "framework/components/anim/animComponent.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/entity.h"
#include "framework/gizmo/gizmoConstants.h"
#include "framework/gizmo/overlay/overlayRenderer.h"

namespace visutwin::canvas
{
    IconGizmo::IconGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : Gizmo(camera, std::move(layer), "gizmo:icon")
    {
        _theme.base = Color(1.0f, 1.0f, 1.0f, 1.0f);
        _theme.occluded = Color(1.0f, 1.0f, 1.0f, 0.2f);
        _theme.backdrop = GIZMO_COLOR_BACKDROP;

        // the base constructor sized the root as a transform gizmo is; the icons are sized by
        // themselves
        if (_root) {
            _root->setLocalScale(1.0f, 1.0f, 1.0f);
        }
        _renderer = std::make_unique<OverlayRenderer>(this,
            OverlayRendererTheme{&_theme.base, &_theme.occluded, nullptr, &_theme.backdrop});

        on(EVENT_POINTERDOWN, [this](float, float, MeshInstance* meshInstance) {
            if (GraphNode* node = _renderer->icons().pick(meshInstance)) {
                fire(EVENT_ICONSELECT, node);
            }
        });
    }

    IconGizmo::~IconGizmo()
    {
        IconGizmo::destroy();
    }

    float IconGizmo::iconSize() const
    {
        return _renderer ? _renderer->iconSize : 24.0f;
    }

    void IconGizmo::setIconSize(const float value)
    {
        if (_renderer) {
            _renderer->iconSize = value;
        }
    }

    void IconGizmo::setTheme(const IconGizmoThemePartial& partial)
    {
        if (partial.base) {
            _theme.base = *partial.base;
        }
        if (partial.occluded) {
            _theme.occluded = *partial.occluded;
        }
        if (partial.backdrop) {
            _theme.backdrop = *partial.backdrop;
        }
    }

    GraphNode* IconGizmo::getIconNode(const MeshInstance* meshInstance) const
    {
        return _renderer ? _renderer->icons().pick(meshInstance) : nullptr;
    }

    std::optional<OverlayIcon> IconGizmo::iconOf(Entity* entity) const
    {
        if (!entity || !entity->enabled()) {
            return std::nullopt;
        }
        auto* camera = entity->findComponent<CameraComponent>();
        if (camera && camera == _camera) {
            return std::nullopt;
        }
        // an entity drawing something can be seen and clicked already
        const auto* render = entity->findComponent<RenderComponent>();
        const auto* element = entity->findComponent<ElementComponent>();
        if ((render && render->enabled() && !render->meshInstances().empty()) || (element && element->enabled())) {
            return std::nullopt;
        }
        if (camera) {
            return OverlayIcon::Camera;
        }
        if (entity->findComponent<LightComponent>()) {
            return OverlayIcon::Light;
        }
        if (entity->findComponent<ParticleSystemComponent>()) {
            return OverlayIcon::ParticleSystem;
        }
        if (entity->findComponent<AnimComponent>()) {
            return OverlayIcon::Anim;
        }
        if (entity->findComponent<AnimationComponent>()) {
            return OverlayIcon::Animation;
        }
        if (entity->findComponent<ScriptComponent>()) {
            return OverlayIcon::Script;
        }
        return std::nullopt;
    }

    void IconGizmo::prerender()
    {
        if (!_renderer) {
            return;
        }
        if (!enabled()) {
            _renderer->clear();
            return;
        }
        _renderer->begin();
        for (GraphNode* node : _nodes) {
            auto* entity = dynamic_cast<Entity*>(node);
            const auto icon = iconOf(entity);
            if (!icon) {
                continue;
            }
            const auto* light = entity->findComponent<LightComponent>();
            _renderer->tint = useLightColor && *icon == OverlayIcon::Light && light ? &light->color() : nullptr;
            _renderer->icons().add(*icon, entity->position(), entity);
        }
        _renderer->tint = nullptr;
        _renderer->end();
    }

    void IconGizmo::destroy()
    {
        if (_renderer) {
            _renderer->destroy();
        }
        Gizmo::destroy();
    }
}
