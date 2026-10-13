// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/shadow-soft.
//
// The low-poly terrain (scaled 30) under the helipad sky (mip 3, turned -70 degrees about
// the vertical), with a large orange box pillar (10 x 130 x 10 at (180, 50, 110)). The
// terrain's three "Icosphere" clouds, which receive no shadow, are each cloned three times
// and the twelve, in a shuffled order, circle the valley at height 4 on radii between 5 and
// 13. A white directional light at (75, 120, 20) casts soft PCSS shadows (bias 0.3, normal
// offset bias 0.2, distance 1000, 2048 map, penumbra size 0.03, falloff 4, 16 samples, 16
// blocker samples). The camera (clear (0.9, 0.9, 0.9), far 1000, ACES) orbits the tree
// "Arbol 2.002" from the direction of (-500, 160, 300), at the distance of 470 upstream sets
// on the first frame.
//
// Keys stand in for upstream's control panel:
//   S        Soft Shadows: PCSS / PCF3 (default PCSS)
//   [ / ]    Resolution: halve / double, 512..4096 (default 2048)
//   - / =    Penumbra: -/+ 0.01, 0..0.2 (default 0.03)
//   , / .    Falloff: -/+ 0.5, 1..10 (default 4)
//   Down/Up  Samples: -/+ 4, 1..128 (default 16)
//   Left/Right  Blocker Samples: -/+ 4, 0..128 (default 16)
//
// @credit Terrain Low Poly by Sketchfab, CC BY 4.0
//
// DEVIATION: upstream's orbitCamera script (inertia 0.2, distanceMax 600) frames the tree on
// start and then eases to a distance of 470; this port starts CameraControls at that final
// pose, with the zoom capped at 600. Its damping is CameraControls' own.
// DEVIATION: a blocker sample count of 0 is taken as 1: the light component clamps it.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kCloudSpeed = 0.2f;
}

class ShadowSoftExample final: public ExampleApp
{
public:
    ShadowSoftExample(): ExampleApp({.title = "Shadow Soft"}) {}

protected:
    bool create() override
    {
        _terrainAsset = std::make_unique<Asset>("terrain", AssetType::CONTAINER, assetPath("models/terrain.glb"));
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* terrainContainer = _terrainAsset->resourceAs<ContainerResource>();
        auto* helipad = _helipadAsset->resourceAs<Texture>();
        if (!terrainContainer || !helipad) {
            spdlog::error("Failed to load the terrain or the helipad env atlas");
            return false;
        }

        // Setup skydome
        scene()->setSkyboxMip(3);
        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.0f, -70.0f, 0.0f));

        // Instantiate the terrain
        auto* terrain = terrainContainer->instantiateRenderEntity();
        terrain->setEngine(engine());
        terrain->setLocalScale(30.0f, 30.0f, 30.0f);
        root()->addChild(terrain);

        // Get the clouds so that we can animate them
        const auto srcClouds = terrain->find([](GraphNode* node) {
            const bool isCloud = node->name().find("Icosphere") != std::string::npos;
            if (isCloud) {
                // No shadow receiving for clouds
                if (auto* entity = dynamic_cast<Entity*>(node)) {
                    if (auto* render = entity->findComponent<RenderComponent>()) {
                        render->setReceiveShadows(false);
                    }
                }
            }
            return isCloud;
        });

        // Clone some additional clouds
        for (auto* node : srcClouds) {
            auto* cloud = dynamic_cast<Entity*>(node);
            if (!cloud || !cloud->parent()) {
                continue;
            }
            _clouds.push_back(cloud);
            for (int i = 0; i < 3; i++) {
                Entity* clone = cloud->clone();
                cloud->parent()->addChild(clone);
                _clouds.push_back(clone);
            }
        }

        // Shuffle the array to give clouds random order
        std::shuffle(_clouds.begin(), _clouds.end(), std::mt19937(12345u));

        // A large orange pillar
        _pillarMaterial = std::make_shared<StandardMaterial>();
        _pillarMaterial->setDiffuse(Color(1.0f, 0.5f, 0.0f, 1.0f));
        auto* pillar = createPrimitive("box", _pillarMaterial.get(), Vector3(180.0f, 50.0f, 110.0f),
            Vector3(10.0f, 130.0f, 10.0f));
        pillar->setName("sphere");

        // Find a tree in the middle to use as a focus point
        Vector3 focusPoint(0.0f, 0.0f, 0.0f);
        if (auto* tree = dynamic_cast<Entity*>(terrain->findByName("Arbol 2.002"))) {
            focusPoint = entityBounds(tree).center();
        } else {
            spdlog::warn("The terrain has no tree named Arbol 2.002");
        }

        // Create an Entity with a camera component, positioned where the orbit script leaves
        // it after the first frame: in the direction of (-500, 160, 300) from the tree, 470
        // away from it
        const Vector3 direction = (Vector3(-500.0f, 160.0f, 300.0f) - focusPoint).normalized();
        auto* camera = createCamera(focusPoint + direction * 470.0f);
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        if (!cameraComponent || !cameraComponent->camera()) {
            spdlog::error("Failed to create the camera");
            return false;
        }
        cameraComponent->camera()->setClearColor(Color(0.9f, 0.9f, 0.9f, 1.0f));
        cameraComponent->camera()->setFarClip(1000.0f);
        cameraComponent->setToneMapping(TONEMAP_ACES);
        if (auto* controls = addOrbitControls(camera, focusPoint)) {
            controls->setZoomRange(Vector2(0.0f, 600.0f));
            controls->storeResetState();
        }

        // Create a directional light casting soft shadows
        auto* light = createDirectionalLight(Vector3(75.0f, 120.0f, 20.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        light->setName("MainLight");
        _light = light->findComponent<LightComponent>();
        if (!_light) {
            spdlog::error("Failed to create the light");
            return false;
        }
        _light->setShadowBias(0.3f);
        _light->setShadowNormalBias(0.2f);
        _light->setShadowDistance(1000.0f);
        applyLightSettings();

        spdlog::info("Keys: S soft shadows, [ ] resolution, - = penumbra, , . falloff, "
            "Down/Up samples, Left/Right blocker samples");
        return true;
    }

    void update(const float dt) override
    {
        handleKeys();

        _time += dt;

        // Move the clouds around
        const auto count = static_cast<float>(_clouds.size());
        for (size_t index = 0; index < _clouds.size(); ++index) {
            const float radialOffset = (static_cast<float>(index) / count) * (6.24f / kCloudSpeed);
            const float radius = 9.0f + 4.0f * std::sin(radialOffset);
            const float cloudTime = _time + radialOffset;
            _clouds[index]->setLocalPosition(2.0f + radius * std::sin(cloudTime * kCloudSpeed), 4.0f,
                -5.0f + radius * std::cos(cloudTime * kCloudSpeed));
        }
    }

    void destroy() override
    {
        // The scene borrows the env atlas; it is torn down after this
        if (scene()) {
            scene()->setEnvAtlas(nullptr);
        }
    }

private:
    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        bool changed = true;
        if (keyboard->wasPressed(Key::S)) {
            _soft = !_soft;
        } else if (keyboard->wasPressed(Key::LeftBracket)) {
            _shadowResolution = std::max(_shadowResolution / 2, 512);
        } else if (keyboard->wasPressed(Key::RightBracket)) {
            _shadowResolution = std::min(_shadowResolution * 2, 4096);
        } else if (keyboard->wasPressed(Key::Minus)) {
            _penumbraSize = std::max(_penumbraSize - 0.01f, 0.0f);
        } else if (keyboard->wasPressed(Key::Equals)) {
            _penumbraSize = std::min(_penumbraSize + 0.01f, 0.2f);
        } else if (keyboard->wasPressed(Key::Comma)) {
            _penumbraFalloff = std::max(_penumbraFalloff - 0.5f, 1.0f);
        } else if (keyboard->wasPressed(Key::Period)) {
            _penumbraFalloff = std::min(_penumbraFalloff + 0.5f, 10.0f);
        } else if (keyboard->wasPressed(Key::Down)) {
            _shadowSamples = std::max(_shadowSamples - 4, 1);
        } else if (keyboard->wasPressed(Key::Up)) {
            _shadowSamples = std::min(_shadowSamples + 4, 128);
        } else if (keyboard->wasPressed(Key::Left)) {
            _shadowBlockerSamples = std::max(_shadowBlockerSamples - 4, 0);
        } else if (keyboard->wasPressed(Key::Right)) {
            _shadowBlockerSamples = std::min(_shadowBlockerSamples + 4, 128);
        } else {
            changed = false;
        }
        if (changed) {
            applyLightSettings();
        }
    }

    // Update properties on the light from the panel's values
    void applyLightSettings()
    {
        _light->setShadowType(_soft ? SHADOW_PCSS_32F : SHADOW_PCF3_32F);
        _light->setShadowResolution(_shadowResolution);
        _light->setPenumbraSize(_penumbraSize);
        _light->setPenumbraFalloff(_penumbraFalloff);
        _light->setShadowSamples(_shadowSamples);
        _light->setShadowBlockerSamples(_shadowBlockerSamples);
        spdlog::info("soft {} resolution {} penumbra {:.3f} falloff {:.1f} samples {} blocker samples {}",
            _soft, _shadowResolution, _penumbraSize, _penumbraFalloff, _shadowSamples, _shadowBlockerSamples);
    }

    std::unique_ptr<Asset> _terrainAsset;
    std::unique_ptr<Asset> _helipadAsset;
    std::shared_ptr<StandardMaterial> _pillarMaterial;
    std::vector<Entity*> _clouds;
    LightComponent* _light = nullptr;
    float _time = 0.0f;

    // Initial settings
    bool _soft = true;
    int _shadowResolution = 2048;
    float _penumbraSize = 0.03f;
    float _penumbraFalloff = 4.0f;
    int _shadowSamples = 16;
    int _shadowBlockerSamples = 16;
};

VISUTWIN_EXAMPLE_MAIN(ShadowSoftExample)
