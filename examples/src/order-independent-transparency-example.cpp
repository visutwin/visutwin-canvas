// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// An original scene: upstream has no order-independent transparency, so there is no example to
// port. It shows what Layer::setOrderIndependentTransparency is for: surfaces no back-to-front
// order of whole objects can draw correctly.
//
// Three large transparent panes (red, green, blue, opacity 0.45) pass through one another at
// the origin, each turned 60 degrees from the next about the vertical axis, so every pane is
// both in front of and behind each of the others. A ring of eight transparent spheres (opacity
// 0.35, a hue each) passes through the panes, and an opaque white box stands at the centre, half
// inside them: whatever is behind it must be hidden, whatever is in front of it must show. The
// floor is a checkerboard (checkboard.png tiled 10 x 10, 20 units square). Ambient 0.35, a
// directional light of 1.2 turned (50, 30, 0), no shadows. The camera orbits the origin at a
// distance of 9, 3 units up.
//
// With sorting (key 1 off) each object is drawn whole, back to front by its centre, so wherever
// two panes cross, one draws entirely over the other on one side of the crossing and wrongly on
// the other. With order-independent transparency (on to begin with) the panes blend the same
// from every angle, and the box still hides what is behind it.
//
// Keys:
//   1  order-independent transparency on the World layer on / off (on)
//   2  render through a camera frame (HDR, into a texture) instead of the back buffer (off)
//   3  pause / resume the orbit
// VISUTWIN_OIT=0 starts with sorting instead (for a capture of each).
//
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kOrbitRadius = 9.0f;
    constexpr float kOrbitHeight = 3.0f;
    constexpr float kOrbitSpeed = 0.25f;   // radians a second

    Color hue(const float h)
    {
        // A fully saturated hue at h in [0, 1).
        const float r = std::clamp(std::abs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
        const float g = std::clamp(2.0f - std::abs(h * 6.0f - 2.0f), 0.0f, 1.0f);
        const float b = std::clamp(2.0f - std::abs(h * 6.0f - 4.0f), 0.0f, 1.0f);
        return Color(r, g, b, 1.0f);
    }
}

class OrderIndependentTransparencyExample final : public ExampleApp
{
public:
    OrderIndependentTransparencyExample() : ExampleApp({.title = "Order-Independent Transparency"}) {}

protected:
    bool create() override
    {
        _checkerboardAsset = std::make_unique<Asset>("checkerboard", AssetType::TEXTURE,
            assetPath("textures/checkboard.png"));
        Texture* checkerboard = _checkerboardAsset->resourceAs<Texture>();
        if (!checkerboard) {
            spdlog::error("Failed to load textures/checkboard.png");
            return false;
        }

        scene()->setAmbientLight(0.35f, 0.35f, 0.35f);
        scene()->lighting().shadowsEnabled = false;

        auto floorMaterial = std::make_shared<StandardMaterial>();
        floorMaterial->setDiffuseMap(checkerboard);
        floorMaterial->setDiffuseMapTiling(Vector2(10.0f, 10.0f));
        floorMaterial->setGloss(0.0f);
        floorMaterial->setUseMetalness(false);
        Entity* floor = createPrimitive("plane", floorMaterial.get(), Vector3(0.0f, -1.5f, 0.0f),
            Vector3(20.0f, 1.0f, 20.0f));
        floor->setName("Floor");
        _materials.push_back(floorMaterial);

        auto boxMaterial = std::make_shared<StandardMaterial>();
        boxMaterial->setDiffuse(Color(0.9f, 0.9f, 0.9f, 1.0f));
        boxMaterial->setUseMetalness(true);
        boxMaterial->setMetalness(0.0f);
        boxMaterial->setGloss(0.5f);
        Entity* box = createPrimitive("box", boxMaterial.get(), Vector3(0.0f, -0.5f, 0.0f), Vector3(1.2f, 2.0f, 1.2f));
        box->setName("Box");
        _materials.push_back(boxMaterial);

        // Three panes through the origin, 60 degrees apart about the vertical axis.
        const std::array<Color, 3> paneColors = {Color(1.0f, 0.15f, 0.1f, 1.0f), Color(0.1f, 1.0f, 0.2f, 1.0f),
            Color(0.15f, 0.35f, 1.0f, 1.0f)};
        for (size_t i = 0; i < paneColors.size(); ++i) {
            auto material = createTransparentMaterial(paneColors[i], 0.45f);
            Entity* pane = createPrimitive("plane", material.get(), Vector3(0.0f, 0.5f, 0.0f), Vector3(5.0f, 1.0f, 3.5f));
            pane->setName("Pane-" + std::to_string(i));
            pane->setLocalEulerAngles(90.0f, static_cast<float>(i) * 60.0f, 0.0f);
        }

        // A ring of spheres through the panes.
        constexpr int sphereCount = 8;
        for (int i = 0; i < sphereCount; ++i) {
            const float angle = static_cast<float>(i) / sphereCount * 2.0f * std::numbers::pi_v<float>;
            auto material = createTransparentMaterial(hue(static_cast<float>(i) / sphereCount), 0.35f);
            Entity* sphere = createPrimitive("sphere", material.get(),
                Vector3(std::cos(angle) * 1.8f, 0.5f + std::sin(angle * 2.0f) * 0.6f, std::sin(angle) * 1.8f),
                Vector3(1.3f, 1.3f, 1.3f));
            sphere->setName("Sphere-" + std::to_string(i));
        }

        createDirectionalLight(Vector3(50.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.2f);

        Entity* camera = createCamera(Vector3(0.0f, kOrbitHeight, kOrbitRadius));
        _cameraEntity = camera;
        _camera = camera->findComponent<CameraComponent>();
        _camera->camera()->setClearColor(Color(0.08f, 0.09f, 0.11f, 1.0f));
        placeCamera();

        _worldLayer = scene()->layers()->getLayerById(LAYERID_WORLD);
        if (const char* value = std::getenv("VISUTWIN_OIT"); value && *value) {
            _oit = value[0] != '0';
        }
        applySettings();
        spdlog::info("Keys: 1 order-independent transparency, 2 camera frame, 3 pause the orbit");
        return true;
    }

    void update(const float dt) override
    {
        if (!_paused) {
            _orbitAngle += dt * kOrbitSpeed;
            placeCamera();
        }
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        if (keyboard->wasPressed(Key::Digit1)) {
            _oit = !_oit;
        } else if (keyboard->wasPressed(Key::Digit2)) {
            _cameraFrame = !_cameraFrame;
        } else if (keyboard->wasPressed(Key::Digit3)) {
            _paused = !_paused;
            return;
        } else {
            return;
        }
        applySettings();
    }

private:
    std::shared_ptr<StandardMaterial> createTransparentMaterial(const Color& color, const float opacity)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setUseMetalness(true);
        material->setMetalness(0.0f);
        material->setGloss(0.7f);
        material->setOpacity(opacity);
        material->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        material->setTransparent(true);
        material->setCullMode(CullMode::CULLFACE_NONE);
        material->setTwoSidedLighting(true);
        auto depth = std::make_shared<DepthState>();
        depth->setDepthWrite(false);
        material->setDepthState(depth);
        _materials.push_back(material);
        return material;
    }

    void placeCamera()
    {
        _cameraEntity->setLocalPosition(std::sin(_orbitAngle) * kOrbitRadius, kOrbitHeight,
            std::cos(_orbitAngle) * kOrbitRadius);
        _cameraEntity->lookAt(Vector3(0.0f, 0.2f, 0.0f));
    }

    void applySettings()
    {
        if (_worldLayer) {
            _worldLayer->setOrderIndependentTransparency(_oit);
        }
        auto rendering = _camera->rendering();
        rendering.useCameraFrame = _cameraFrame;
        _camera->setRendering(rendering);
        spdlog::info("Order-independent transparency {}, rendering to {}", _oit ? "on" : "off",
            _cameraFrame ? "a camera frame" : "the back buffer");
    }

    std::unique_ptr<Asset> _checkerboardAsset;
    Entity* _cameraEntity = nullptr;
    CameraComponent* _camera = nullptr;
    std::shared_ptr<Layer> _worldLayer;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;

    float _orbitAngle = 0.6f;
    bool _oit = true;
    bool _cameraFrame = false;
    bool _paused = false;
};

VISUTWIN_EXAMPLE_MAIN(OrderIndependentTransparencyExample)
