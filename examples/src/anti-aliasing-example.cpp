// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.10.2026
//
// Port of upstream camera-frame/anti-aliasing.
//
// The scene of camera-frame/taa — the PBR house (scaled 100x) under the table-mountain env
// atlas (skybox mip 0, exposure 2.5), a shadow-casting directional light, a cube orbiting the
// house — plus a sign of thin slanted light bars on a dark board in front of the house, a high
// contrast test of how well each method smooths edges. The camera frame tone maps with ACES;
// SMAA (SmaaEffect) is registered on the camera and enabled when a method uses it.
//
// Keys stand in for upstream's controls panel, over the same parameters:
//   M      the method, cycled: none, SMAA low / medium / high / ultra, TAA, TAA upscale (from
//          the render target scale to the full resolution), MSAA 4x, MSAA 4x + SMAA high
//          (starts at SMAA high)
//   D      the SMAA debug view, cycled: none, edges, weights (shown while SMAA is in use)
//   - / =  render target scale -/+ 0.05, 0.25..1
//   S      sharpness, cycled 0 / 0.25 / 0.5 / 0.75 / 1
//   B      bloom on (0.02) / off
// VISUTWIN_AA_METHOD=n and VISUTWIN_AA_DEBUG=n start at method n and debug view n of those
// lists, for a capture, which no key reaches.
//
// DEVIATIONS:
// - upstream's orbitCamera script is replaced by the examples' CameraControls in orbit mode,
//   the start pose computed as frameOnStart does it (see taa-example.cpp). There is no inertia.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/graphics/smaaEffect.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    struct SmaaPreset
    {
        float threshold;
        int maxSearchSteps;
        int maxSearchStepsDiag;
        float cornerRounding;
    };

    // The quality presets of SMAA: low, medium, high, ultra.
    constexpr std::array<SmaaPreset, 4> kSmaaPresets = {{
        {0.15f, 4, 0, 100.0f},
        {0.1f, 8, 0, 100.0f},
        {0.1f, 16, 8, 25.0f},
        {0.05f, 32, 16, 25.0f},
    }};

    struct Method
    {
        const char* name;
        int smaaPreset;   // -1: no SMAA
        bool taa;
        bool upscale;
        int samples;
    };

    constexpr std::array<Method, 9> kMethods = {{
        {"None", -1, false, false, 1},
        {"SMAA - Low", 0, false, false, 1},
        {"SMAA - Medium", 1, false, false, 1},
        {"SMAA - High", 2, false, false, 1},
        {"SMAA - Ultra", 3, false, false, 1},
        {"TAA", -1, true, false, 1},
        {"TAA - Upscale", -1, true, true, 1},
        {"MSAA 4x", -1, false, false, 4},
        {"MSAA 4x + SMAA - High", 2, false, false, 4},
    }};

    constexpr std::array<const char*, 3> kDebugViews = {"none", "edges", "weights"};
}

class AntiAliasingExample final: public ExampleApp
{
public:
    // The camera frame applies the selected anti-aliasing, multisampling included.
    AntiAliasingExample(): ExampleApp({.title = "Anti-Aliasing Example", .antialias = false}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _house = std::make_unique<Asset>("house", AssetType::CONTAINER, assetPath("models/pbr-house.glb"));
        _cube = std::make_unique<Asset>("cube", AssetType::CONTAINER, assetPath("models/playcanvas-cube.glb"));

        // Setup skydome with low intensity
        if (Texture* envAtlasTexture = _envAtlas->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlasTexture);
        }
        scene()->setSkyboxMip(0);
        scene()->setExposure(2.5f);

        // Create an instance of the house and add it to the scene
        ContainerResource* houseContainer = _house->resourceAs<ContainerResource>();
        if (!houseContainer) {
            spdlog::error("Failed to load house model");
            return false;
        }
        auto* houseEntity = houseContainer->instantiateRenderEntity();
        houseEntity->setLocalScale(100, 100, 100);
        root()->addChild(houseEntity);

        // Frame the house the way frameOnStart does.
        constexpr float fov = 80.0f;
        const auto houseBbox = entityBounds(houseEntity);
        const Vector3 pivot = houseBbox.center();
        const Vector3 halfExtents = houseBbox.halfExtents();
        const float radius = std::max({halfExtents.getX(), halfExtents.getY(), halfExtents.getZ()});
        const float distance = std::min(radius * 1.5f / std::sin(0.5f * fov * DEG_TO_RAD), 400.0f);
        const Vector3 authoredPosition(0.0f, 40.0f, -220.0f);
        const Vector3 cameraPosition = pivot + (authoredPosition - pivot).normalized() * distance;

        auto* camera = createCamera(cameraPosition);
        _cameraComp = camera->findComponent<CameraComponent>();
        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->camera()->setNearClip(10.0f);
            _cameraComp->camera()->setFarClip(600.0f);
            _cameraComp->camera()->setFov(fov);
        }
        if (auto* controls = addOrbitControls(camera, pivot)) {
            controls->setZoomRange(Vector2(0.0f, 400.0f));
            controls->storeResetState();
        }

        // A sign of thin slanted bars in front of the house, a high contrast test of how well each
        // method smooths the edges. It stands still, facing the camera as it starts: TAA
        // reprojects a pixel's history from the camera's motion alone.
        _darkMaterial = createUnlitMaterial(Color(0.1f, 0.1f, 0.1f, 1.0f));
        _lightMaterial = createUnlitMaterial(Color(0.6f, 0.6f, 0.6f, 1.0f));

        auto* pattern = new Entity();
        pattern->setEngine(engine());
        pattern->setName("Pattern");
        pattern->setLocalPosition(60.0f, 56.0f, -112.0f);
        pattern->setLocalEulerAngles(169.0f, 0.0f, 180.0f);
        pattern->setLocalScale(4.0f, 4.0f, 4.0f);
        root()->addChild(pattern);

        pattern->addChild(createPrimitive("box", _darkMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(14.0f, 9.0f, 0.1f)));
        for (int i = 0; i < 13; ++i) {
            auto* bar = createPrimitive("box", _lightMaterial.get(), Vector3(-6.0f + static_cast<float>(i), 0.0f, 0.1f),
                Vector3(0.08f, 8.0f, 0.08f));
            bar->setLocalEulerAngles(0.0f, 0.0f, 17.0f);
            pattern->addChild(bar);
        }

        // Add a shadow casting directional light
        auto* light = createDirectionalLight(Vector3(40.0f, 10.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setRange(700.0f);
            lightComp->setShadowResolution(4096);
            lightComp->setShadowDistance(600.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        if (ContainerResource* cubeContainer = _cube->resourceAs<ContainerResource>()) {
            _cubeEntity = cubeContainer->instantiateRenderEntity();
            _cubeEntity->setLocalScale(30, 30, 30);
            root()->addChild(_cubeEntity);
        }

        if (_cameraComp) {
            _cameraComp->setToneMapping(TONEMAP_ACES);
            // SMAA is not part of the camera frame by default: it is added, and enabled when
            // selected.
            _smaa = std::make_shared<SmaaEffect>();
            _cameraComp->addFrameEffect(_smaa);
        }

        if (const char* method = std::getenv("VISUTWIN_AA_METHOD")) {
            _method = std::clamp(std::atoi(method), 0, static_cast<int>(kMethods.size()) - 1);
        }
        if (const char* debug = std::getenv("VISUTWIN_AA_DEBUG")) {
            _debug = std::clamp(std::atoi(debug), 0, static_cast<int>(kDebugViews.size()) - 1);
        }
        spdlog::info("Controls: M method, D SMAA debug view, -/= resolution scale, S sharpness, B bloom");
        applySettings();
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_M:
            _method = (_method + 1) % static_cast<int>(kMethods.size());
            break;
        case SDLK_D:
            _debug = (_debug + 1) % static_cast<int>(kDebugViews.size());
            break;
        case SDLK_MINUS:
            _scale = std::max(0.25f, _scale - 0.05f);
            break;
        case SDLK_EQUALS:
            _scale = std::min(1.0f, _scale + 0.05f);
            break;
        case SDLK_S:
            _sharpness = _sharpness >= 0.875f ? 0.0f : _sharpness + 0.25f;
            break;
        case SDLK_B:
            _bloom = !_bloom;
            break;
        default:
            return false;
        }
        applySettings();
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        if (_cubeEntity) {
            _cubeEntity->setLocalPosition(130.0f * std::sin(_time), 0.0f, 130.0f * std::cos(_time));
            _cubeEntity->rotate(50.0f * dt, 20.0f * dt, 30.0f * dt);
        }
    }

private:
    static std::shared_ptr<StandardMaterial> createUnlitMaterial(const Color& color)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setEmissive(color);
        material->setUseLighting(false);
        return material;
    }

    void applySettings()
    {
        if (!_cameraComp || !_smaa) {
            return;
        }
        const Method& method = kMethods[static_cast<size_t>(_method)];

        auto rendering = _cameraComp->rendering();
        rendering.bloomIntensity = _bloom ? 0.02f : 0.0f;
        rendering.renderTargetScale = _scale;
        rendering.sharpness = _sharpness;
        rendering.samples = method.samples;

        // The SMAA debug views: the edges it found and how much it blends the pixels.
        const bool smaa = method.smaaPreset >= 0;
        rendering.composeDebug = smaa && _debug > 0 ? std::string("smaa") + kDebugViews[static_cast<size_t>(_debug)]
                                                    : std::string();
        _cameraComp->setRendering(rendering);

        auto taa = _cameraComp->taa();
        taa.enabled = method.taa;
        taa.upscale = method.upscale;
        _cameraComp->setTaa(taa);

        _smaa->setEnabled(smaa);
        if (smaa) {
            const SmaaPreset& preset = kSmaaPresets[static_cast<size_t>(method.smaaPreset)];
            _smaa->threshold = preset.threshold;
            _smaa->maxSearchSteps = preset.maxSearchSteps;
            _smaa->maxSearchStepsDiag = preset.maxSearchStepsDiag;
            _smaa->cornerRounding = preset.cornerRounding;
        }

        spdlog::info("method={}, debug={}, scale={:.2f}, sharpness={:.2f}, bloom={}", method.name,
            kDebugViews[static_cast<size_t>(_debug)], _scale, _sharpness, _bloom ? "on" : "off");
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _house;
    std::unique_ptr<Asset> _cube;
    std::shared_ptr<StandardMaterial> _darkMaterial;
    std::shared_ptr<StandardMaterial> _lightMaterial;
    std::shared_ptr<SmaaEffect> _smaa;

    CameraComponent* _cameraComp = nullptr;
    Entity* _cubeEntity = nullptr;

    int _method = 3;   // SMAA - High
    int _debug = 0;
    float _scale = 1.0f;
    float _sharpness = 0.0f;
    bool _bloom = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(AntiAliasingExample)
