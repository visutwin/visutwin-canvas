// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Physics raycast probe: two horizontal rays sweep up and down through a row of
// static collision shapes. raycastFirst() paints only the nearest hit red;
// raycastAll() paints every shape it passes through. Blue markers show the
// surface normal at each hit.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/math/quaternion.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

void orientYAxisToDirection(Entity* entity, const Vector3& direction)
{
    if (!entity || direction.lengthSquared() < 1e-8f) {
        return;
    }

    const Vector3 from = Vector3(0.0f, 1.0f, 0.0f);
    const Vector3 to = direction.normalized();
    const float dot = std::clamp(from.dot(to), -1.0f, 1.0f);

    Quaternion rotation;
    if (dot > 0.9999f) {
        rotation = Quaternion::fromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    } else if (dot < -0.9999f) {
        rotation = Quaternion::fromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 180.0f);
    } else {
        const Vector3 axis = from.cross(to).normalized();
        const float angleDeg = std::acos(dot) * RAD_TO_DEG;
        rotation = Quaternion::fromAxisAngle(axis, angleDeg);
    }

    entity->setLocalRotation(rotation);
}

void setSegmentMarker(Entity* marker, const Vector3& start, const Vector3& end, const float thickness)
{
    if (!marker) {
        return;
    }

    const Vector3 delta = end - start;
    const float length = delta.length();
    if (length <= 1e-5f) {
        marker->setLocalScale(Vector3(0.0f, 0.0f, 0.0f));
        return;
    }

    marker->setLocalPosition((start + end) * 0.5f);
    orientYAxisToDirection(marker, delta);
    marker->setLocalScale(Vector3(thickness, length, thickness));
}

class RaycastExample final: public ExampleApp
{
public:
    RaycastExample()
        : ExampleApp({.title = "Physics Raycast Probe", .width = 1200, .height = 760}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
    }

    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        _green = std::make_shared<StandardMaterial>();
        _green->setDiffuse(Color(0.0f, 1.0f, 0.0f, 1.0f));

        _red = std::make_shared<StandardMaterial>();
        _red->setDiffuse(Color(1.0f, 0.0f, 0.0f, 1.0f));

        _white = std::make_shared<StandardMaterial>();
        _white->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _white->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _white->setEmissiveIntensity(8.0f);

        _blue = std::make_shared<StandardMaterial>();
        _blue->setDiffuse(Color(0.0f, 0.2f, 1.0f, 1.0f));
        _blue->setEmissive(Color(0.0f, 0.2f, 1.0f, 1.0f));
        _blue->setEmissiveIntensity(8.0f);

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 2.0f);

        auto* cameraEntity = createCamera(Vector3(5.0f, 0.0f, 15.0f));
        if (auto* camera = cameraEntity->findComponent<CameraComponent>();
            camera && camera->camera()) {
            camera->camera()->setClearColor(Color(0.5f, 0.5f, 0.8f, 1.0f));
        }
        cameraEntity->lookAt(Vector3(5.0f, 0.0f, 0.0f));

        const std::vector<std::string> types = {"box", "capsule", "cone", "cylinder", "sphere"};
        _physicalRenders.reserve(types.size() * 2u);

        for (const float y : {2.0f, -2.0f}) {
            for (size_t i = 0; i < types.size(); ++i) {
                auto* entity = createPhysicalShape(
                    types[i], _green.get(), Vector3(static_cast<float>(i) * 2.0f + 1.0f, y, 0.0f));
                if (auto* render = entity->findComponent<RenderComponent>()) {
                    _physicalRenders.push_back(render);
                }
            }
        }

        _rayFirstMarker = createPrimitive("cylinder", _white.get(),
            Vector3(0.0f, 2.0f, 0.0f), Vector3(0.03f, 1.0f, 0.03f));
        _rayAllMarker = createPrimitive("cylinder", _white.get(),
            Vector3(0.0f, -2.0f, 0.0f), Vector3(0.03f, 1.0f, 0.03f));

        // DEVIATION: upstream uses app.drawLine for debug rays/normals; this native sample visualizes
        // them with thin cylinder render primitives until immediate line rendering is ported.
        _normalMarkers.reserve(16);
        for (int i = 0; i < 16; ++i) {
            _normalMarkers.push_back(createPrimitive("cylinder", _blue.get(),
                Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f)));
        }

        _rigidbodySystem = dynamic_cast<RigidBodyComponentSystem*>(engine()->systems()->getById("rigidbody"));
        if (!_rigidbodySystem) {
            spdlog::error("RigidBodyComponentSystem not available");
            return false;
        }

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        const float time = _time;

        for (auto* render : _physicalRenders) {
            if (!render) {
                continue;
            }
            render->setMaterial(_green.get());
        }

        for (auto* marker : _normalMarkers) {
            marker->setLocalScale(Vector3(0.0f, 0.0f, 0.0f));
        }

        const float yFirst = 2.0f + 1.2f * std::sin(time);
        const Vector3 startFirst(0.0f, yFirst, 0.0f);
        const Vector3 endFirst(10.0f, yFirst, 0.0f);
        setSegmentMarker(_rayFirstMarker, startFirst, endFirst, 0.03f);

        int normalMarkerCursor = 0;
        if (const auto hit = _rigidbodySystem->raycastFirst(startFirst, endFirst); hit.has_value()) {
            if (auto* render = hit->entity ? hit->entity->findComponent<RenderComponent>() : nullptr) {
                render->setMaterial(_red.get());
            }

            if (normalMarkerCursor < static_cast<int>(_normalMarkers.size())) {
                const Vector3 normalEnd = hit->point + hit->normal * 0.8f;
                setSegmentMarker(_normalMarkers[normalMarkerCursor], hit->point, normalEnd, 0.04f);
                normalMarkerCursor++;
            }
        }

        const float yAll = -2.0f + 1.2f * std::sin(time);
        const Vector3 startAll(0.0f, yAll, 0.0f);
        const Vector3 endAll(10.0f, yAll, 0.0f);
        setSegmentMarker(_rayAllMarker, startAll, endAll, 0.03f);

        const auto allHits = _rigidbodySystem->raycastAll(startAll, endAll);
        for (const auto& hit : allHits) {
            if (auto* render = hit.entity ? hit.entity->findComponent<RenderComponent>() : nullptr) {
                render->setMaterial(_red.get());
            }

            if (normalMarkerCursor < static_cast<int>(_normalMarkers.size())) {
                const Vector3 normalEnd = hit.point + hit.normal * 0.8f;
                setSegmentMarker(_normalMarkers[normalMarkerCursor], hit.point, normalEnd, 0.04f);
                normalMarkerCursor++;
            }
        }
    }

private:
    Entity* createPhysicalShape(const std::string& type, StandardMaterial* material,
        const Vector3& position)
    {
        auto* entity = createPrimitive(type.c_str(), material, position, Vector3(1.0f, 1.0f, 1.0f));

        if (auto* rigidbody = static_cast<RigidBodyComponent*>(entity->addComponent<RigidBodyComponent>())) {
            rigidbody->setType("static");
        }

        if (auto* collision = static_cast<CollisionComponent*>(entity->addComponent<CollisionComponent>())) {
            collision->setType(type);
            if (type == "capsule") {
                collision->setHeight(2.0f);
            }
        }

        return entity;
    }

    std::shared_ptr<StandardMaterial> _green;
    std::shared_ptr<StandardMaterial> _red;
    std::shared_ptr<StandardMaterial> _white;
    std::shared_ptr<StandardMaterial> _blue;

    std::vector<RenderComponent*> _physicalRenders;
    std::vector<Entity*> _normalMarkers;
    Entity* _rayFirstMarker = nullptr;
    Entity* _rayAllMarker = nullptr;

    RigidBodyComponentSystem* _rigidbodySystem = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(RaycastExample)
