// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
// DEVIATION: Rendering replaced with ImGui-based overlays. This file now
// only handles annotation registration, screen-space projection, hover
// detection, and click handling. All visual rendering is external.
//
#include "annotationManager.h"
#include "annotation.h"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

#include "spdlog/spdlog.h"

#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/components/camera/cameraComponent.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    namespace
    {
        // The visible annotation whose screen position is nearest (x, y), strictly
        // closer than maxDistance; nullptr if there is none.
        Annotation* closestAnnotation(const std::vector<AnnotationScreenInfo>& infos,
            const float x, const float y, float maxDistance)
        {
            Annotation* closest = nullptr;
            const Vector2 point(x, y);
            for (const auto& info : infos) {
                if (!info.visible) continue;

                const float dist = (point - Vector2(info.screenX, info.screenY)).length();
                if (dist < maxDistance) {
                    maxDistance = dist;
                    closest = info.annotation;
                }
            }
            return closest;
        }
    }

    AnnotationManager::~AnnotationManager()
    {
        // The engine can outlive this script; leaving the [this] subscriptions
        // behind would dispatch into freed memory on the next annotation event.
        if (_onAnnotationAdd) {
            _onAnnotationAdd->off();
        }
        if (_onAnnotationRemove) {
            _onAnnotationRemove->off();
        }
    }

    void AnnotationManager::initialize()
    {
        auto* eng = entity()->engine();
        if (!eng) return;

        // Camera is found lazily in update() since it may not exist at initialize time
        findCameraEntity();

        // Listen for annotation add/remove events on the engine
        _onAnnotationAdd = eng->on("annotation:add", [this](Annotation* annotation) {
            registerAnnotation(annotation);
        });
        _onAnnotationRemove = eng->on("annotation:remove", [this](Annotation* annotation) {
            unregisterAnnotation(annotation);
        });

        spdlog::info("AnnotationManager initialized");
    }

    void AnnotationManager::registerAnnotation(Annotation* annotation)
    {
        // Avoid duplicates
        auto it = std::find(_annotations.begin(), _annotations.end(), annotation);
        if (it != _annotations.end()) return;

        _annotations.push_back(annotation);
        spdlog::info("Registered annotation: label='{}' title='{}'", annotation->label(), annotation->title());
    }

    void AnnotationManager::unregisterAnnotation(Annotation* annotation)
    {
        auto it = std::find(_annotations.begin(), _annotations.end(), annotation);
        if (it == _annotations.end()) return;

        if (_activeAnnotation == annotation) {
            _activeAnnotation = nullptr;
        }
        if (_hoveredAnnotation == annotation) {
            _hoveredAnnotation = nullptr;
        }

        _annotations.erase(it);
    }

    bool AnnotationManager::worldToScreen(const Vector3& worldPos, float& screenX, float& screenY) const
    {
        if (!_camera) return false;

        auto* cameraComp = _camera->findComponent<CameraComponent>();
        if (!cameraComp || !cameraComp->camera()) return false;

        // Get view matrix from camera's world transform inverse
        const Matrix4 viewMatrix = _camera->worldTransform().inverse();
        const Matrix4& projMatrix = cameraComp->camera()->projectionMatrix();

        // Transform to view space
        Vector3 viewPos = viewMatrix.transformPoint(worldPos);

        // Check if behind camera (view space Z is positive = behind camera in right-hand coords)
        // In our column-major layout, after view transform, negative Z is in front
        if (viewPos.getZ() >= 0.0f) {
            return false;
        }

        // Transform to clip space
        Vector4 clipPos = projMatrix * Vector4(viewPos, 1.0f);

        if (std::abs(clipPos.getW()) < 1e-6f) return false;

        // NDC coordinates
        const Vector3 ndc = clipPos.perspectiveDivide();
        float ndcX = ndc.getX();
        float ndcY = ndc.getY();

        // Get screen dimensions
        int windowW = 0, windowH = 0;
        auto* eng = entity()->engine();
        if (eng && eng->sdlWindow()) {
            SDL_GetWindowSize(eng->sdlWindow(), &windowW, &windowH);
        }
        if (windowW <= 0 || windowH <= 0) return false;

        // Convert NDC to screen coordinates
        // NDC X: -1 (left) to +1 (right) -> screen 0 to windowW
        // NDC Y: -1 (bottom) to +1 (top) -> screen windowH to 0 (screen Y is top-down)
        screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(windowW);
        screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(windowH);

        return true;
    }

    void AnnotationManager::findCameraEntity()
    {
        if (_camera) return;

        auto* eng = entity()->engine();
        if (!eng) return;

        // DEVIATION: upstream finds camera via findComponent; we search the scene graph
        auto root = eng->root();
        if (!root) return;

        std::function<Entity*(Entity*)> search = [&](Entity* e) -> Entity* {
            if (e->findComponent<CameraComponent>()) return e;
            for (const auto& child : e->children()) {
                auto* ent = dynamic_cast<Entity*>(child.get());
                if (!ent) continue;
                auto* found = search(ent);
                if (found) return found;
            }
            return nullptr;
        };
        _camera = search(root);
    }

    void AnnotationManager::update(float dt)
    {
        // Lazily find camera if not yet available
        if (!_camera) {
            findCameraEntity();
        }
        if (!_camera) return;

        // Rebuild screen-space info for all annotations
        _screenInfos.clear();
        _screenInfos.reserve(_annotations.size());

        for (auto* annotation : _annotations) {
            if (!annotation->enabled()) continue;

            AnnotationScreenInfo info;
            info.annotation = annotation;

            const Vector3 worldPos = annotation->entity()->position();
            info.visible = worldToScreen(worldPos, info.screenX, info.screenY);

            _screenInfos.push_back(info);
        }
    }

    void AnnotationManager::updateHover(float mouseX, float mouseY)
    {
        _hoveredAnnotation = closestAnnotation(_screenInfos, mouseX, mouseY, _hotspotSize + 5.0f);
    }

    void AnnotationManager::handleClick(float screenX, float screenY)
    {
        Annotation* const closest = closestAnnotation(_screenInfos, screenX, screenY, _hotspotSize + 5.0f);

        if (closest) {
            if (_activeAnnotation == closest) {
                // Toggle off
                _activeAnnotation = nullptr;
                spdlog::info("Annotation hidden: '{}'", closest->title());
            } else {
                _activeAnnotation = closest;
                spdlog::info("Annotation selected: '{}' -- {}", closest->title(), closest->text());
            }
        } else {
            _activeAnnotation = nullptr;
        }
    }
}
