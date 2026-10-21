// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
#include "boundingBoxGizmo.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/componentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/gizmo/gizmoMaterial.h"
#include "platform/graphics/depthState.h"
#include "scene/camera.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        float component(const Vector3& v, const int axis)
        {
            return axis == 0 ? v.getX() : (axis == 1 ? v.getY() : v.getZ());
        }

        Vector3 withComponent(const Vector3& v, const int axis, const float value)
        {
            return Vector3(axis == 0 ? value : v.getX(), axis == 1 ? value : v.getY(), axis == 2 ? value : v.getZ());
        }

        std::shared_ptr<GizmoMaterial> createArmMaterial(const std::shared_ptr<GraphicsDevice>& device,
            const bool depthTest)
        {
            auto material = std::make_shared<GizmoMaterial>(device);
            auto depth = std::make_shared<DepthState>();
            depth->setDepthTest(depthTest);
            depth->setDepthWrite(false);
            material->setDepthState(depth);
            return material;
        }
    }

    std::array<BoundingBoxGizmo::Arm, BoundingBoxGizmo::kArmCount> BoundingBoxGizmo::armTransforms(
        const ArmLayout& layout)
    {
        std::array<Arm, kArmCount> arms{};
        const Vector3& c = layout.center;
        const Vector3& h = layout.halfExtents;
        size_t i = 0;
        for (int x = -1; x <= 1; x += 2) {
            for (int y = -1; y <= 1; y += 2) {
                for (int z = -1; z <= 1; z += 2) {
                    const Vector3 corner(c.getX() + h.getX() * static_cast<float>(x),
                        c.getY() + h.getY() * static_cast<float>(y), c.getZ() + h.getZ() * static_cast<float>(z));
                    const float depth = layout.perspective
                        ? std::max((corner - layout.cameraPosition).dot(layout.cameraForward), layout.nearClip)
                        : 1.0f;
                    const float thickness = layout.armThickness * layout.pixel * depth;
                    const int signs[3] = {x, y, z};
                    for (int k = 0; k < 3; ++k) {
                        const float length = component(h, k) * layout.armLength;
                        Vector3 position = corner - layout.rootPosition;
                        position = withComponent(position, k, component(position, k) -
                            static_cast<float>(signs[k]) * 0.5f * (length - 0.5f * thickness));
                        const float along = length + 0.5f * thickness;
                        arms[i++] = Arm{position, Vector3(k == 0 ? along : thickness, k == 1 ? along : thickness,
                            k == 2 ? along : thickness)};
                    }
                }
            }
        }
        return arms;
    }

    BoundingBoxGizmo::BoundingBoxGizmo(CameraComponent* camera, std::shared_ptr<Layer> layer)
        : Gizmo(camera, layer, "gizmo:bounding-box")
    {
        // The base sized the root to the viewport before this class existed.
        if (_root) {
            _root->setLocalScale(1.0f, 1.0f, 1.0f);
        }
        _scale = 1.0f;

        Engine* engine = this->engine();
        if (!engine || !engine->graphicsDevice() || !_root || !layer) {
            return;
        }
        const auto& device = engine->graphicsDevice();
        _occludedMaterial = createArmMaterial(device, false);
        _material = createArmMaterial(device, true);
        _mesh = createMeshFromGeometry(device, createBoxGeometry());

        auto box = std::make_unique<Entity>();
        _box = box.get();
        _box->setEngine(engine);
        _box->setName("box");
        _root->addChild(std::move(box));

        // The occluded pass first, then the depth tested pass over it: the layer keeps
        // collection order.
        for (int i = 0; i < kArmCount; ++i) {
            auto arm = std::make_unique<Entity>();
            Entity* entity = arm.get();
            entity->setEngine(engine);
            entity->setName("arm:" + std::to_string(i));
            _box->addChild(std::move(arm));
            auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
            if (!render) {
                continue;
            }
            render->setLayers({layer->id()});
            render->setCastShadows(false);
            for (const auto& material : {_occludedMaterial, _material}) {
                auto instance = std::make_unique<MeshInstance>(_mesh, std::static_pointer_cast<Material>(material),
                    entity);
                instance->setCull(false);
                instance->setCastShadow(false);
                render->addMeshInstance(std::move(instance));
            }
            _arms[static_cast<size_t>(i)] = entity;
        }
    }

    BoundingBoxGizmo::~BoundingBoxGizmo() = default;

    void BoundingBoxGizmo::setTheme(const BoundingBoxGizmoThemePartial& partial)
    {
        if (partial.base) {
            _theme.base = *partial.base;
        }
        if (partial.occluded) {
            _theme.occluded = *partial.occluded;
        }
    }

    void BoundingBoxGizmo::addNode(GraphNode* node)
    {
        if (!node || !node->enabled()) {
            return;
        }
        if (auto* entity = dynamic_cast<Entity*>(node)) {
            if (auto* render = entity->findComponent<RenderComponent>(); render && render->enabled()) {
                for (MeshInstance* meshInstance : render->meshInstances()) {
                    if (!meshInstance || !meshInstance->visible()) {
                        continue;
                    }
                    const BoundingBox aabb = meshInstance->aabb();
                    if (_bounds) {
                        _bounds->add(aabb);
                    } else {
                        _bounds = aabb;
                    }
                }
            }
        }
        node->forEachChild([this](GraphNode* child) { addNode(child); });
    }

    void BoundingBoxGizmo::prerender()
    {
        if (!enabled() || !_box) {
            return;
        }

        _bounds.reset();
        for (GraphNode* node : _nodes) {
            addNode(node);
        }
        _box->setEnabled(_bounds.has_value());
        if (!_bounds) {
            return;
        }

        if (_occludedMaterial && _material) {
            _occludedMaterial->setColor(_theme.occluded);
            _material->setColor(_theme.base);
        }

        // The world size of a canvas point, per unit of depth in perspective.
        const Camera* camera = _camera ? _camera->camera() : nullptr;
        if (!camera) {
            return;
        }
        ArmLayout layout;
        layout.center = _bounds->center();
        layout.halfExtents = _bounds->halfExtents() + Vector3(padding, padding, padding);
        layout.rootPosition = _root->position();
        layout.armLength = armLength;
        layout.armThickness = armThickness;
        layout.perspective = camera->projection() == ProjectionType::Perspective;
        const auto [width, height] = _engine ? _engine->canvasSize() : std::pair<int, int>{1, 1};
        const float span = layout.perspective
            ? 2.0f * std::tan(0.5f * camera->fov() * std::numbers::pi_v<float> / 180.0f)
            : 2.0f * camera->orthoHeight();
        layout.pixel = span / static_cast<float>(std::max(camera->horizontalFov() ? width : height, 1));
        layout.cameraPosition = cameraPosition();
        layout.cameraForward = cameraForward();
        layout.nearClip = camera->nearClip();

        const auto arms = armTransforms(layout);
        for (size_t i = 0; i < arms.size(); ++i) {
            if (Entity* arm = _arms[i]) {
                arm->setLocalPosition(arms[i].position);
                arm->setLocalScale(arms[i].scale.getX(), arms[i].scale.getY(), arms[i].scale.getZ());
            }
        }
    }
}
