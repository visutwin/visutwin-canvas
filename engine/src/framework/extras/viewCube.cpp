// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.07.2026.
//
#include "viewCube.h"

#include <cmath>
#include <numbers>

#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "platform/graphics/depthState.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr float kAxisLength = 1.0f;

        const Vector3 kAxes[6] = {
            Vector3(1.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, 0.0f, 1.0f),
            Vector3(-1.0f, 0.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f), Vector3(0.0f, 0.0f, -1.0f)
        };
    }

    ViewCube::ViewCube(Engine* engine) : _engine(engine)
    {
        _root = new Entity();
        _root->setEngine(engine);
        engine->root()->addChild(_root);

        setColors(Color(0.86f, 0.16f, 0.16f, 1.0f), Color(0.24f, 0.72f, 0.19f, 1.0f),
            Color(0.16f, 0.35f, 0.9f, 1.0f), Color(0.3f, 0.3f, 0.3f, 1.0f));
    }

    ViewCube::~ViewCube()
    {
        if (_root) {
            if (_root->parent()) {
                auto ownedRoot = _root->remove();
                ownedRoot.reset();
            } else {
                delete _root;
            }
            _root = nullptr;
        }
        _handles.fill(nullptr);
        _rods.fill(nullptr);
        _materials.fill(nullptr);
    }

    Entity* ViewCube::makeHandle(const Color& color, const float radius)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setName("view-cube-handle");
        material->setUseLighting(false);
        material->setDiffuse(color);
        // Drawn on the Immediate layer with depth testing off so the gizmo always
        // stays visible on top of the scene.
        auto depthState = std::make_shared<DepthState>();
        depthState->setDepthTest(false);
        depthState->setDepthWrite(false);
        material->setDepthState(depthState);

        auto* entity = new Entity();
        entity->setEngine(_engine);
        entity->setLocalScale(radius * 2.0f, radius * 2.0f, radius * 2.0f);
        if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
            render->setLayers({LAYERID_IMMEDIATE});
            render->setMaterial(material.get());
            render->setType("sphere");
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }
        _root->addChild(entity);

        // keep material alive
        for (auto& slot : _materials) {
            if (!slot) {
                slot = material;
                break;
            }
        }
        return entity;
    }

    Entity* ViewCube::makeRod(const Color& color)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setName("view-cube-rod");
        material->setUseLighting(false);
        material->setDiffuse(color);
        auto depthState = std::make_shared<DepthState>();
        depthState->setDepthTest(false);
        depthState->setDepthWrite(false);
        material->setDepthState(depthState);

        auto* entity = new Entity();
        entity->setEngine(_engine);
        if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
            render->setLayers({LAYERID_IMMEDIATE});
            render->setMaterial(material.get());
            render->setType("box");
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }
        _root->addChild(entity);

        for (auto& slot : _materials) {
            if (!slot) {
                slot = material;
                break;
            }
        }
        return entity;
    }

    void ViewCube::setColors(const Color& x, const Color& y, const Color& z, const Color& negative)
    {
        // Rebuild the gnomon: 3 positive handles (filled axis color), 3 negative
        // handles (neutral fill), 3 axis rods from the center to the positive handles.
        for (auto*& handle : _handles) {
            if (handle) {
                auto removed = _root->removeChild(handle);
                removed.reset();
                handle = nullptr;
            }
        }
        for (auto*& rod : _rods) {
            if (rod) {
                auto removed = _root->removeChild(rod);
                removed.reset();
                rod = nullptr;
            }
        }
        _materials.fill(nullptr);

        const Color axisColors[3] = {x, y, z};
        constexpr float rodThickness = 0.07f;

        for (int i = 0; i < 3; ++i) {
            _handles[i] = makeHandle(axisColors[i], _handleRadius);
            const Vector3 p = kAxes[i] * (kAxisLength * _lineLength);
            _handles[i]->setLocalPosition(p.getX(), p.getY(), p.getZ());

            _handles[i + 3] = makeHandle(negative, _handleRadius * 0.8f);
            const Vector3 n = kAxes[i + 3] * (kAxisLength * _lineLength);
            _handles[i + 3]->setLocalPosition(n.getX(), n.getY(), n.getZ());

            _rods[i] = makeRod(axisColors[i]);
            const Vector3 mid = kAxes[i] * (kAxisLength * _lineLength * 0.5f);
            _rods[i]->setLocalPosition(mid.getX(), mid.getY(), mid.getZ());
            const Vector3 scale = Vector3(rodThickness, rodThickness, rodThickness)
                + kAxes[i] * (kAxisLength * _lineLength - rodThickness);
            _rods[i]->setLocalScale(std::abs(scale.getX()), std::abs(scale.getY()), std::abs(scale.getZ()));
        }
    }

    void ViewCube::update(Entity* cameraEntity)
    {
        if (!cameraEntity || !_root) {
            return;
        }
        const auto* cameraComponent = cameraEntity->findComponent<CameraComponent>();
        const auto* camera = cameraComponent ? cameraComponent->camera() : nullptr;
        if (!camera) {
            return;
        }

        const Matrix4& wt = cameraEntity->worldTransform();
        const Vector3 camPos = wt.getTranslation();
        const Vector3 right = Vector3(1.0f, 0.0f, 0.0f).transformNormal(wt).normalized();
        const Vector3 up = Vector3(0.0f, 1.0f, 0.0f).transformNormal(wt).normalized();
        const Vector3 forward = Vector3(0.0f, 0.0f, -1.0f).transformNormal(wt).normalized();

        // Place the gnomon in the top-right corner at a fixed distance in front of
        // the camera, sized as a fraction of the vertical field of view.
        const float distance = 10.0f;
        const float fovRad = camera->fov() * (std::numbers::pi_v<float> / 180.0f);
        const float halfHeight = distance * std::tan(fovRad * 0.5f);
        const float aspect = camera->aspectRatio() > 0.0f ? camera->aspectRatio() : 1.0f;
        const float halfWidth = halfHeight * aspect;
        const float gnomonScale = halfHeight * _screenSize * 0.5f;
        const float margin = gnomonScale * 1.8f;

        const Vector3 anchor = camPos + forward * distance
            + right * (halfWidth - margin) + up * (halfHeight - margin);
        _root->setLocalPosition(anchor.getX(), anchor.getY(), anchor.getZ());
        _root->setLocalScale(gnomonScale, gnomonScale, gnomonScale);
        // World-aligned rotation: the gnomon shows the world axes.
        _root->setLocalRotation(Quaternion());
    }

    std::optional<Vector3> ViewCube::onClick(const float x, const float y,
        const float screenWidth, const float screenHeight, Entity* cameraEntity)
    {
        if (!cameraEntity || screenWidth <= 0.0f || screenHeight <= 0.0f) {
            return std::nullopt;
        }
        auto* cameraComponent = cameraEntity->findComponent<CameraComponent>();
        auto* camera = cameraComponent ? cameraComponent->camera() : nullptr;
        if (!camera) {
            return std::nullopt;
        }

        // Build a picking ray through the click point (engine standard-Z clip space,
        // near plane at clip z = 0).
        const Matrix4& wt = cameraEntity->worldTransform();
        const Vector3 camPos = wt.getTranslation();
        const Matrix4 viewProjection = const_cast<Camera*>(camera)->projectionMatrix() * wt.inverse();
        const Matrix4 invViewProjection = viewProjection.inverse();

        const float ndcX = (2.0f * x / screenWidth) - 1.0f;
        const float ndcY = 1.0f - (2.0f * y / screenHeight);
        const Vector4 farPoint = invViewProjection * Vector4(ndcX, ndcY, 1.0f, 1.0f);
        if (std::abs(farPoint.getW()) < 1e-8f) {
            return std::nullopt;
        }
        const Vector3 target(farPoint.getX() / farPoint.getW(),
            farPoint.getY() / farPoint.getW(), farPoint.getZ() / farPoint.getW());
        const Vector3 dir = (target - camPos).normalized();

        // Nearest ray-sphere hit against the six handles.
        float bestT = -1.0f;
        int bestIndex = -1;
        for (int i = 0; i < 6; ++i) {
            if (!_handles[i]) {
                continue;
            }
            const Vector3 center = _handles[i]->worldTransform().getTranslation();
            const float radius = _handleRadius * _root->worldTransform().getElement(0, 0) * 2.4f;

            const Vector3 toCenter = center - camPos;
            const float tClosest = toCenter.dot(dir);
            if (tClosest <= 0.0f) {
                continue;
            }
            const Vector3 closest = camPos + dir * tClosest;
            if ((center - closest).lengthSquared() <= radius * radius) {
                if (bestT < 0.0f || tClosest < bestT) {
                    bestT = tClosest;
                    bestIndex = i;
                }
            }
        }

        if (bestIndex < 0) {
            return std::nullopt;
        }
        const Vector3 axis = kAxes[bestIndex];
        fire(EVENT_CAMERAALIGN, axis);
        return axis;
    }
}
