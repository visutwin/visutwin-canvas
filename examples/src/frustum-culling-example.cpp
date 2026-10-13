// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream debug/frustum-culling.
//
// Forty boxes and spheres drift inside a 24 m cube, bouncing off its walls, while an observer
// camera (fov 45, near 2, far 26, a manual aspect of 1.6, not rendering) circles them looking at
// the middle. Each object is tested against the observer's frustum and hidden when outside it,
// as the renderer culls; its bounds and its direction of travel are drawn green when inside and
// red when outside. The observer's frustum and axes are drawn yellow, the cube and the
// observer's path grey. The main camera watches from outside, or the scene can be viewed through
// the observer to see that nothing pops in.
//
// Keys standing in for upstream's controls panel:
//   C  apply culling on / off      F  frustum on / off       B  bounds on / off
//   V  velocity on / off           O  view through the observer on / off
//
// DEVIATION: the objects' colours, places and velocities come from a fixed-seed generator
// rather than Math.random(), so every run starts the same.
//
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "framework/components/render/renderComponent.h"
#include "framework/extras/wireRenderer.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/frustumUtils.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kHalfExtent = 12.0f;
    constexpr float kOrbitRadius = 26.0f;
    constexpr float kOrbitHeight = 8.0f;
    constexpr float kPi = 3.14159265358979f;
}

class FrustumCullingExample final : public ExampleApp
{
public:
    FrustumCullingExample() : ExampleApp({.title = "Frustum Culling"}) {}

protected:
    bool create() override
    {
        // Renderers for the overlay: a second set of settings is simply a second renderer, and
        // renderers sharing a layer and depth-test mode draw into one batch.
        _wire.emplace(scene().get());
        _visibleWire.emplace(scene().get());
        _visibleWire->setColor(Color(0.2f, 1.0f, 0.4f, 1.0f));
        _culledWire.emplace(scene().get());
        _culledWire->setColor(Color(1.0f, 0.25f, 0.25f, 1.0f));

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f);

        // A cloud of drifting objects, each keeping its own velocity
        std::mt19937 random(20261013u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        for (int i = 0; i < 40; ++i) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.4f + unit(random) * 0.5f, 0.4f + unit(random) * 0.5f, 0.5f, 1.0f));
            material->setGloss(0.4f);
            material->setMetalness(0.2f);
            material->setUseMetalness(true);
            _materials.push_back(material);
            const Vector3 position((unit(random) * 2.0f - 1.0f) * kHalfExtent, (unit(random) * 2.0f - 1.0f) * kHalfExtent,
                (unit(random) * 2.0f - 1.0f) * kHalfExtent);
            Entity* entity = createPrimitive(i % 3 == 0 ? "box" : "sphere", material.get(), position,
                Vector3(1.6f, 1.6f, 1.6f));
            const Vector3 velocity = Vector3(unit(random) * 2.0f - 1.0f, unit(random) * 2.0f - 1.0f,
                unit(random) * 2.0f - 1.0f).normalized() * (2.0f + unit(random) * 4.0f);
            _objects.push_back({entity, velocity});
        }

        // The camera the scene is viewed from
        _camera = createCamera(Vector3(45.0f, 28.0f, 45.0f));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        if (auto* component = _camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.05f, 0.06f, 0.08f, 1.0f));
            component->camera()->setFarClip(200.0f);
        }

        // The camera whose frustum is drawn and culled against. Its aspect is set by hand so the
        // frustum is defined while the camera is not rendering.
        _observer = createCamera(Vector3(0.0f, kOrbitHeight, kOrbitRadius));
        _observerCamera = _observer->findComponent<CameraComponent>();
        if (_observerCamera && _observerCamera->camera()) {
            Camera* camera = _observerCamera->camera();
            camera->setFov(45.0f);
            camera->setNearClip(2.0f);
            camera->setFarClip(26.0f);
            camera->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
            camera->setAspectRatio(1.6f);
            camera->setClearColor(Color(0.05f, 0.06f, 0.08f, 1.0f));
        }
        _observerCamera->setEnabled(false);

        // The path the observer travels, drawn as a closed loop
        for (int i = 0; i < 64; ++i) {
            const float angle = static_cast<float>(i) / 64.0f * kPi * 2.0f;
            _orbitPath.emplace_back(std::sin(angle) * kOrbitRadius, kOrbitHeight, std::cos(angle) * kOrbitRadius);
        }

        spdlog::info("Keys: C culling, F frustum, B bounds, V velocity, O observer view");
        return true;
    }

    void update(const float dt) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            if (keyboard->wasPressed(Key::C)) _applyCulling = !_applyCulling;
            if (keyboard->wasPressed(Key::F)) _showFrustum = !_showFrustum;
            if (keyboard->wasPressed(Key::B)) _showBounds = !_showBounds;
            if (keyboard->wasPressed(Key::V)) _showVelocity = !_showVelocity;
            if (keyboard->wasPressed(Key::O)) _throughObserver = !_throughObserver;
        }
        _time += dt;

        // Drift the objects, reflecting them off the walls of the region
        for (Object& object : _objects) {
            Vector3 position = object.entity->localPosition() + object.velocity * dt;
            Vector3& v = object.velocity;
            if (position.getX() < -kHalfExtent || position.getX() > kHalfExtent) v = Vector3(-v.getX(), v.getY(), v.getZ());
            if (position.getY() < -kHalfExtent || position.getY() > kHalfExtent) v = Vector3(v.getX(), -v.getY(), v.getZ());
            if (position.getZ() < -kHalfExtent || position.getZ() > kHalfExtent) v = Vector3(v.getX(), v.getY(), -v.getZ());
            object.entity->setLocalPosition(position);
        }

        // Sweep the observer around the scene, always looking at the middle of the region
        _observer->setLocalPosition(std::sin(_time * 0.3f) * kOrbitRadius, kOrbitHeight,
            std::cos(_time * 0.3f) * kOrbitRadius);
        _observer->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // The frustum the objects are tested against, built as the renderer builds it
        const Frustum frustum = buildCameraFrustum(_observerCamera->camera(), _observer);
        for (Object& object : _objects) {
            auto* render = object.entity->findComponent<RenderComponent>();
            MeshInstance* meshInstance = render && !render->meshInstances().empty() ? render->meshInstances()[0] : nullptr;
            if (!meshInstance) {
                continue;
            }
            const BoundingBox aabb = meshInstance->aabb();
            const bool inside = isVisibleInFrustum(frustum, aabb);
            // Culling an object is hiding it, which is what the renderer does
            meshInstance->setVisible(_applyCulling ? inside : true);
            WireRenderer& wire = inside ? *_visibleWire : *_culledWire;
            if (_showBounds) {
                wire.box(aabb);
            }
            if (_showVelocity) {
                // The direction only, so a fast object does not draw a longer arrow
                const Vector3 origin = object.entity->position();
                wire.arrow(origin, origin + object.velocity.normalized() * 3.0f);
            }
        }

        if (_showFrustum) {
            _wire->setColor(Color(1.0f, 0.85f, 0.2f, 1.0f));
            _wire->frustum(_observerCamera);
            _wire->axes(_observer->worldTransform(), 3.0f);
        }

        // The region the objects are confined to, and the path the observer travels
        _wire->setColor(Color(0.35f, 0.35f, 0.4f, 1.0f));
        _wire->boxMinMax(Vector3(-kHalfExtent, -kHalfExtent, -kHalfExtent), Vector3(kHalfExtent, kHalfExtent, kHalfExtent));
        _wire->loop(_orbitPath);

        // Seen through the observer, the culling is proven correct: nothing pops in
        if (auto* component = _camera->findComponent<CameraComponent>()) {
            component->setEnabled(!_throughObserver);
        }
        _observerCamera->setEnabled(_throughObserver);
    }

private:
    struct Object
    {
        Entity* entity = nullptr;
        Vector3 velocity;
    };

    std::optional<WireRenderer> _wire;
    std::optional<WireRenderer> _visibleWire;
    std::optional<WireRenderer> _culledWire;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Object> _objects;
    std::vector<Vector3> _orbitPath;
    Entity* _camera = nullptr;
    Entity* _observer = nullptr;
    CameraComponent* _observerCamera = nullptr;
    float _time = 0.0f;
    bool _applyCulling = true;
    bool _showFrustum = true;
    bool _showBounds = true;
    bool _showVelocity = true;
    bool _throughObserver = false;
};

VISUTWIN_EXAMPLE_MAIN(FrustumCullingExample)
