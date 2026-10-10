// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream graphics/clustered-omni-shadows.
//
// A closed 800 x 400 x 800 room (floor, ceiling and four walls built from boxes) holds
// seven towers of eight randomly tumbled 25-unit cubes, on two alternating radii. Ten
// white omni lights, each marked by a small emissive sphere, circle the room on a
// tilted ring; every one casts a cubemap shadow and projects the christmas cubemap as
// a cookie. Under clustered lighting each omni renders its six faces into its own slot
// of the packed shadow atlas, and its cookie is copied into the matching slot of the
// cookie atlas. Every surface has the same grey, normal-mapped metalness material;
// the camera (ACES, fov 80) orbits the room's centre.
//
// Keys (upstream's settings panel):
//   1      toggle shadows (Shadows On)
//   2      toggle cookies (Cookies On)
//   - / =  shadow atlas resolution down / up by 256, within 512..4096 (Shadow Res)
//   R reset the camera, F1 statistics, Esc quit; drag to orbit, wheel to zoom.
//
// DEVIATIONS:
// - No Filter selector. Upstream offers PCF1/3/5 in 16F and 32F; this engine has no
//   16-bit shadow formats, and its clustered path has one kernel (a spot or omni light
//   filters with the 3x3 whatever its type), so the panel's default, PCF3_32F, is the
//   only filter there is to show.
// - The towers' tumble comes from a fixed-seed generator instead of Math.random(), so
//   every run shows the same towers and screenshots are comparable.
// - Upstream's orbit camera focuses the bounding box of everything in the scene when
//   it starts, the room's centre (0, 200, 0), and keeps the authored camera position;
//   this port orbits that point from the same position. distanceMax 1200 becomes the
//   zoom range; CameraControls has no inertia factor.
// - The atlas debug overlay upstream draws on WebGPU is left out.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/graphics/texture.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kLightCount = 10;
    constexpr int kTowerCount = 7;
    constexpr float PI_F = 3.14159265358979323846f;

    constexpr int kMinAtlasResolution = 512;
    constexpr int kMaxAtlasResolution = 4096;
    constexpr int kAtlasResolutionStep = 256;

    // The cookie cubemap's faces, in the engine's cube order +X, -X, +Y, -Y, +Z, -Z.
    const std::array<const char*, 6> kXmasFaceFiles = {
        "xmas_posx", "xmas_negx", "xmas_posy", "xmas_negy", "xmas_posz", "xmas_negz"
    };
}

class ClusteredOmniShadowsExample final: public ExampleApp
{
public:
    ClusteredOmniShadowsExample()
        : ExampleApp({.title = "Clustered Omni Shadows"}) {}

protected:
    bool create() override
    {
        _normalMap = std::make_unique<Asset>("normal", AssetType::TEXTURE,
            assetPath("textures/normal-map.png"));

        // Clustered lighting, sized to handle many lights.
        scene()->setClusteredLightingEnabled(true);
        auto& lighting = scene()->lighting();
        lighting.cellsX = 16;
        lighting.cellsY = 12;
        lighting.cellsZ = 16;
        lighting.maxLightsPerCell = 12;
        lighting.shadowsEnabled = true;
        lighting.cookiesEnabled = true;
        lighting.shadowAtlasResolution = 1300;
        lighting.cookieAtlasResolution = 2048;

        // The floor and ceiling.
        createRoomPrimitive(Vector3(0.0f, 0.0f, 0.0f), Vector3(800.0f, 2.0f, 800.0f));
        createRoomPrimitive(Vector3(0.0f, 400.0f, 0.0f), Vector3(800.0f, 2.0f, 800.0f));

        // The walls.
        createRoomPrimitive(Vector3(400.0f, 200.0f, 0.0f), Vector3(2.0f, 400.0f, 800.0f));
        createRoomPrimitive(Vector3(-400.0f, 200.0f, 0.0f), Vector3(2.0f, 400.0f, 800.0f));
        createRoomPrimitive(Vector3(0.0f, 200.0f, 400.0f), Vector3(800.0f, 400.0f, 0.0f));
        createRoomPrimitive(Vector3(0.0f, 200.0f, -400.0f), Vector3(800.0f, 400.0f, 0.0f));

        // Seven towers of eight tumbled cubes, alternating between two radii.
        std::uniform_real_distribution<float> angle(0.0f, 360.0f);
        for (int i = 0; i < kTowerCount; ++i) {
            constexpr float scale = 25.0f;
            const float fraction = (static_cast<float>(i) / kTowerCount) * PI_F * 2.0f;
            const float radius = (i % 2) ? 340.0f : 210.0f;
            for (int y = 0; y <= 7; ++y) {
                Entity* cube = createRoomPrimitive(
                    Vector3(radius * std::sin(fraction), 2.0f + static_cast<float>(y) * 25.0f,
                        radius * std::cos(fraction)),
                    Vector3(scale, scale, scale));
                const float ex = angle(_rng);
                const float ey = angle(_rng);
                const float ez = angle(_rng);
                cube->setLocalEulerAngles(ex, ey, ez);
            }
        }

        // The omni cookie. No mipmaps: only the top level is copied into the
        // clustered cookie atlas.
        AssetData xmasData{.mipmaps = false};
        for (size_t i = 0; i < kXmasFaceFiles.size(); ++i) {
            xmasData.faces[i] = assetPath("cubemaps/xmas_faces/" + std::string(kXmasFaceFiles[i]) + ".png");
        }
        _xmasAsset = std::make_unique<Asset>("xmas_cubemap", AssetType::CUBEMAP, "", xmasData);
        Texture* xmasCookie = _xmasAsset->resourceAs<Texture>();
        if (!xmasCookie) {
            spdlog::warn("xmas cubemap failed to build; the omni lights keep a plain falloff");
        }

        for (int i = 0; i < kLightCount; ++i) {
            createOmniLight(xmasCookie);
        }

        auto* camera = createCamera(Vector3(300.0f, 120.0f, 25.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setFov(80.0f);
            comp->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
            comp->camera()->setFarClip(1500.0f);
            comp->setToneMapping(TONEMAP_ACES);
        }
        // The centre of the room's bounds.
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 200.0f, 0.0f))) {
            controls->setZoomRange(Vector2(0.01f, 1200.0f));
            controls->storeResetState();
        }

        spdlog::info("{} shadow-casting omni lights with cubemap cookies, each in its own "
                     "slot of the clustered shadow and cookie atlases.", kLightCount);
        spdlog::info("Keys: 1 shadows on/off | 2 cookies on/off | -/= shadow atlas resolution | "
                     "R reset camera | Esc quit");
        logSettings();
        return true;
    }

    void update(const float dt) override
    {
        handleKeys();

        _time += dt * 0.3f;
        constexpr float radius = 250.0f;
        for (size_t i = 0; i < _lights.size(); ++i) {
            const float fraction =
                (static_cast<float>(i) / static_cast<float>(_lights.size())) * PI_F * 2.0f;
            _lights[i]->setPosition(
                radius * std::sin(_time + fraction),
                190.0f + std::sin(_time + fraction) * 150.0f,
                radius * std::cos(_time + fraction));
        }
    }

private:
    // A box with its own copy of the grey, normal-mapped material, parented to the root.
    Entity* createRoomPrimitive(const Vector3& position, const Vector3& scale)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(0.7f, 0.7f, 0.7f, 1.0f));

        material->setNormalMap(_normalMap->resourceAs<Texture>());
        material->setNormalMapTiling(Vector2(5.0f, 5.0f));
        material->setBumpiness(0.7f);

        material->setGloss(0.4f);
        material->setMetalness(0.3f);
        material->setUseMetalness(true);
        _materials.push_back(material);

        return createPrimitive("box", material.get(), position, scale);
    }

    void createOmniLight(Texture* cookie)
    {
        auto* entity = new Entity();
        entity->setName("Omni");
        entity->setEngine(engine());

        if (auto* light = static_cast<LightComponent*>(
                entity->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_OMNI);
            light->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            light->setIntensity(10.0f / kLightCount);
            light->setRange(350.0f);
            light->setCastShadows(true);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.2f);
            light->setCookie(cookie);
            light->setCookieChannel(CookieChannel::COOKIE_CHANNEL_RGB);
        }

        // A small emissive sphere marks the light.
        auto material = std::make_shared<StandardMaterial>();
        material->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _materials.push_back(material);
        if (auto* render = static_cast<RenderComponent*>(
                entity->addComponent<RenderComponent>())) {
            render->setMaterial(material.get());
            render->setType("sphere");
            render->setCastShadows(false);
        }

        entity->setPosition(0.0f, 120.0f, 0.0f);
        entity->setLocalScale(5.0f, 5.0f, 5.0f);
        root()->addChild(entity);
        _lights.push_back(entity);
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        auto& lighting = scene()->lighting();
        bool changed = false;
        if (keyboard->wasPressed(Key::Digit1)) {
            lighting.shadowsEnabled = !lighting.shadowsEnabled;
            changed = true;
        }
        if (keyboard->wasPressed(Key::Digit2)) {
            lighting.cookiesEnabled = !lighting.cookiesEnabled;
            changed = true;
        }
        if (keyboard->wasPressed(Key::Minus)) {
            lighting.shadowAtlasResolution = std::max(kMinAtlasResolution,
                lighting.shadowAtlasResolution - kAtlasResolutionStep);
            changed = true;
        }
        if (keyboard->wasPressed(Key::Equals)) {
            lighting.shadowAtlasResolution = std::min(kMaxAtlasResolution,
                lighting.shadowAtlasResolution + kAtlasResolutionStep);
            changed = true;
        }
        if (changed) {
            logSettings();
        }
    }

    void logSettings()
    {
        const auto& lighting = scene()->lighting();
        spdlog::info("Shadows {} | cookies {} | shadow atlas {}",
            lighting.shadowsEnabled ? "on" : "off", lighting.cookiesEnabled ? "on" : "off",
            lighting.shadowAtlasResolution);
    }

    std::unique_ptr<Asset> _normalMap;
    std::unique_ptr<Asset> _xmasAsset;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _lights;
    std::mt19937 _rng{20261010};
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ClusteredOmniShadowsExample)
