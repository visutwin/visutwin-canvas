// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.11.2026
//
#include "extras/script/animUpdateOptimizer.h"

#include <algorithm>
#include <any>
#include <cmath>
#include <unordered_map>

#include <spdlog/spdlog.h>

#include "core/shape/boundingBox.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/component.h"
#include "framework/components/componentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/frustumUtils.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        // Each camera's frustum, built once a frame (by render version) and shared by every
        // script using the camera; keyed on the camera entity's id, which is never reused.
        struct CameraFrustum
        {
            int frame = -1;
            Frustum frustum{};
        };

        const Frustum& frustumFor(Entity* cameraEntity, Camera* camera, const int frame)
        {
            static std::unordered_map<EntityId, CameraFrustum> frustums;
            if (frustums.size() > 64) {
                std::erase_if(frustums, [frame](const auto& entry) { return entry.second.frame != frame; });
            }
            CameraFrustum& entry = frustums[cameraEntity->id()];
            if (entry.frame != frame) {
                entry.frame = frame;
                entry.frustum = buildCameraFrustum(camera, cameraEntity);
            }
            return entry.frustum;
        }

        bool warnedNoCustomAabb = false;

        bool inHierarchy(const GraphNode* node, const GraphNode* root)
        {
            for (; node; node = node->parent()) {
                if (node == root) {
                    return true;
                }
            }
            return false;
        }
    }

    AnimUpdateOptimizer::AnimUpdateOptimizer() = default;

    AnimUpdateOptimizer::~AnimUpdateOptimizer()
    {
        if (_renderAddHandle) {
            _renderAddHandle->off();
        }
        if (_renderRemoveHandle) {
            _renderRemoveHandle->off();
        }
    }

    void AnimUpdateOptimizer::setCamera(Entity* camera)
    {
        _camera = camera;
        _cameraWatch.watch(camera, [this]() { _camera = nullptr; });
    }

    void AnimUpdateOptimizer::initialize()
    {
        // Drop the cached render components when one joins or leaves this character.
        Engine* engine = entity() ? entity()->engine() : nullptr;
        if (engine && engine->systems()) {
            if (auto* renderSystem = engine->systems()->getByComponentType<RenderComponent>()) {
                const auto onRenderChanged = [this](const EventArgs& args) {
                    if (_renders.empty() || args.empty()) {
                        return;
                    }
                    const auto* changed = std::any_cast<Entity*>(&args[0]);
                    if (!changed || inHierarchy(*changed, entity())) {
                        _renders.clear();
                    }
                };
                _renderAddHandle = renderSystem->on("add", onRenderChanged);
                _renderRemoveHandle = renderSystem->on("beforeremove", onRenderChanged);
            }
        }

        // Give the anim component its defaults back when the script stops steering it.
        on("disable", [this]() { restoreDefaults(); });
        on("destroy", [this]() { restoreDefaults(); });
    }

    void AnimUpdateOptimizer::restoreDefaults()
    {
        if (auto* anim = entity() ? entity()->findComponent<AnimComponent>() : nullptr) {
            anim->setOffscreenUpdate(AnimOffscreenUpdate::FULL);
            anim->setEvaluationInterval(1);
        }
        _visible = false;
        _renders.clear();
    }

    void AnimUpdateOptimizer::update(float /*dt*/)
    {
        auto* anim = entity() ? entity()->findComponent<AnimComponent>() : nullptr;
        if (!anim) {
            return;
        }

        auto* cameraComponent = _camera ? _camera->findComponent<CameraComponent>() : nullptr;
        Entity* cameraEntity = cameraComponent && cameraComponent->camera() ? _camera : nullptr;

        _visible = _predict && cameraEntity && predictVisible(cameraEntity);
        anim->setOffscreenUpdate(_visible ? AnimOffscreenUpdate::FULL : _offscreenUpdate);

        int interval = 1;
        if (cameraEntity && _lodDistance > 0.0f) {
            const float distance = (entity()->position() - cameraEntity->position()).length();
            interval = std::min(_maxEvaluationInterval, 1 + static_cast<int>(std::floor(distance / _lodDistance)));
        }
        anim->setEvaluationInterval(interval);
    }

    bool AnimUpdateOptimizer::predictVisible(Entity* cameraEntity)
    {
        Engine* engine = entity()->engine();
        const int frame = engine && engine->graphicsDevice() ? engine->graphicsDevice()->renderVersion() : 0;
        Camera* camera = cameraEntity->findComponent<CameraComponent>()->camera();
        const Frustum& frustum = frustumFor(cameraEntity, camera, frame);

        // Searched again while none were found, as the model may be added after the script.
        if (_renders.empty()) {
            _renders = entity()->findComponents<RenderComponent>();
        }

        const Vector3 margin(_margin, _margin, _margin);
        BoundingBox worldAabb;
        for (RenderComponent* render : _renders) {
            if (!render) {
                continue;
            }
            for (MeshInstance* meshInstance : render->meshInstances()) {
                if (!meshInstance) {
                    continue;
                }
                if (render->customAabb() && meshInstance->node()) {
                    worldAabb.setFromTransformedAabb(*render->customAabb(), meshInstance->node()->worldTransform());
                } else {
                    if (!warnedNoCustomAabb) {
                        warnedNoCustomAabb = true;
                        spdlog::warn("AnimUpdateOptimizer: the render component of {} has no custom AABB; the "
                            "bounds of its mesh instances are used instead, which is slower for skinned meshes",
                            render->entity() ? render->entity()->name() : std::string("an entity"));
                    }
                    worldAabb = meshInstance->aabb();
                }
                worldAabb.setHalfExtents(worldAabb.halfExtents() + margin);
                if (isVisibleInFrustum(frustum, worldAabb)) {
                    return true;
                }
            }
        }
        return false;
    }

    void AnimUpdateOptimizer::cloneFrom(const Script& source)
    {
        const auto* src = dynamic_cast<const AnimUpdateOptimizer*>(&source);
        if (!src) {
            return;
        }
        setCamera(src->_camera);
        _predict = src->_predict;
        _margin = src->_margin;
        _offscreenUpdate = src->_offscreenUpdate;
        _lodDistance = src->_lodDistance;
        _maxEvaluationInterval = src->_maxEvaluationInterval;
    }

    void AnimUpdateOptimizer::resolveClonedReferences(const Script& /*source*/, const CloneNodeMap& map)
    {
        Entity* remapped = Component::remapCloned(_camera, map);
        if (remapped != _camera) {
            setCamera(remapped);
        }
    }
}

REGISTER_SCRIPT(visutwin::canvas::AnimUpdateOptimizer, "AnimUpdateOptimizer")
