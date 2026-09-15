// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/shadow-cascades.
//
// A low-poly terrain (scaled 30x) under the helipad environment, lit by one white
// directional light casting four cascaded PCF shadow maps over a 1000-unit shadow
// distance. The terrain's clouds (every node whose name contains "Icosphere") stop
// receiving shadows, are each cloned three more times, and circle over the valley
// every frame. The camera orbits a tree in the middle of the terrain ("Arbol 2.002")
// at a distance of 470, under ACES tone mapping and a light-grey clear.
//
// Keys stand in for upstream's control panel, over its parameters and ranges:
//   1-4  cascade count                 D / C  cascade distribution +/- (0..1)
//   B/V  cascade blend +/- (0..0.2)    = / -  shadow resolution x2 / /2 (128..2048)
//   T    cycle shadow filter type      [ / ]  VSM blur size -/+ (1..25)
//   ; / '  PCSS penumbra size -/+ (0..0.2)    , / .  PCSS penumbra falloff -/+ (1..10)
//
// DEVIATIONS:
// - No skybox rotation. Upstream rotates the skybox by euler (0, -70, 0); Scene has no
//   skybox rotation, so the sky and the environment lighting keep their default
//   orientation.
// - No "Every Frame" toggle and no per-cascade shadowUpdateOverrides. Upstream can
//   refresh the nearest cascade every frame and the others every 5/10/15 frames; the
//   engine has no per-cascade update override, and LightComponent::syncToLight forces
//   SHADOWUPDATE_REALTIME on every shadow-casting light each frame. All cascades update
//   every frame, which is upstream's default (everyFrame: true).
// - Filter types. Upstream offers PCF1/3/5 in 16F and 32F, VSM 16F/32F and PCSS; the
//   engine implements PCF1_32F, PCF3_32F, VSM_16F and PCSS_32F, so T cycles those four.
// - shadowSamples and shadowBlockerSamples (16 each upstream) are not exposed on
//   LightComponent, so the PCSS sample counts are the engine's own.
// - The cloud order is shuffled with a fixed seed rather than Math.random(), so the
//   cloud layout is the same on every run and screenshots are comparable.
// - Orbit camera: upstream's orbit-camera script focuses the tree's bounding box, and
//   on the first frame sets the distance to 470 (distanceMax 1800, inertia 0.2). This
//   port orbits the centre of the tree's AABB from the same starting position at 470,
//   clamps zoom to 1800, and CameraControls has no inertia setting.
//
// @credit Terrain Low Poly by Sketchfab, CC BY 4.0
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <framework/assets/asset.h>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class ShadowCascadesExample final: public ExampleApp
{
public:
    ShadowCascadesExample()
        : ExampleApp({.title = "Shadow Cascades Example", .width = 1100, .height = 750}) {}

protected:
    bool create() override
    {
        // Setup skydome
        scene()->setSkyboxMip(3);

        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        const auto envAtlasResource = _envAtlas->resource();
        if (!envAtlasResource) {
            spdlog::error("Failed to load environment atlas texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

        // Instantiate the terrain
        _terrainAsset = std::make_unique<Asset>(
            "terrain", AssetType::CONTAINER, assetPath("models/terrain.glb"));
        const auto terrainResource = _terrainAsset->resource();
        if (!terrainResource || !std::holds_alternative<ContainerResource*>(*terrainResource)) {
            spdlog::error("Failed to load terrain.glb");
            return false;
        }
        auto* terrain = std::get<ContainerResource*>(*terrainResource)->instantiateRenderEntity();
        if (!terrain) {
            spdlog::error("Failed to instantiate terrain.glb");
            return false;
        }
        terrain->setEngine(engine());
        terrain->setLocalScale(30.0f, 30.0f, 30.0f);
        root()->addChild(terrain);

        // Get the clouds so that we can animate them; no shadow receiving for clouds.
        const auto srcClouds = terrain->find([](GraphNode* node) {
            const bool isCloud = node->name().find("Icosphere") != std::string::npos;
            if (isCloud) {
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

        // Find a tree in the middle to use as a focus point
        Vector3 focusPoint(0.0f, 0.0f, 0.0f);
        if (auto* tree = dynamic_cast<Entity*>(terrain->findByName("Arbol 2.002"))) {
            focusPoint = entityBounds(tree).center();
        } else {
            spdlog::warn("Focus tree 'Arbol 2.002' not found; orbiting the origin");
        }

        // Camera
        auto* camera = createCamera(Vector3(300.0f, 160.0f, 25.0f));
        if (auto* cameraComp = camera->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->camera()->setClearColor(Color(0.9f, 0.9f, 0.9f, 1.0f));
            cameraComp->camera()->setFarClip(1000.0f);
            cameraComp->setToneMapping(TONEMAP_ACES);
        }

        _controls = addOrbitControls(camera, focusPoint);
        if (_controls) {
            _controls->setZoomRange(Vector2(0.0f, 1800.0f));
            // upstream: on the first frame, move the camera further away from the focus tree
            _controls->setOrbitDistance(470.0f);
            _controls->storeResetState();
        }

        // Create a directional light casting cascaded shadows
        auto* dirLight = createDirectionalLight(Vector3(45.0f, 350.0f, 20.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        dirLight->setName("Cascaded Light");
        _light = dirLight->findComponent<LightComponent>();
        if (_light) {
            // Upstream authoring values; see AGENTS.md "Shadow bias convention".
            _light->setShadowBias(0.3f);
            _light->setShadowNormalBias(0.2f);
            _light->setShadowDistance(1000.0f);
            applyLightSettings();
        }

        spdlog::info("Cascade keys: 1-4 count, D/C distribution, B/V blend, =/- resolution, "
            "T filter type, [/] VSM blur, ;/' PCSS penumbra, ,/. PCSS falloff");
        logSettings("init");
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Move the clouds around
        const auto count = static_cast<float>(_clouds.size());
        for (size_t index = 0; index < _clouds.size(); ++index) {
            const float radialOffset = (static_cast<float>(index) / count) * (6.24f / kCloudSpeed);
            const float radius = 9.0f + 4.0f * std::sin(radialOffset);
            const float cloudTime = _time + radialOffset;
            _clouds[index]->setLocalPosition(
                2.0f + radius * std::sin(cloudTime * kCloudSpeed),
                4.0f,
                -5.0f + radius * std::cos(cloudTime * kCloudSpeed));
        }
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_light) {
            return false;
        }

        const char* what = nullptr;
        switch (event.key.key) {
        case SDLK_1:
        case SDLK_2:
        case SDLK_3:
        case SDLK_4:
            _numCascades = static_cast<int>(event.key.key - SDLK_1) + 1;
            what = "count";
            break;
        case SDLK_D:
            _cascadeDistribution = std::min(1.0f, _cascadeDistribution + 0.05f);
            what = "distribution";
            break;
        case SDLK_C:
            _cascadeDistribution = std::max(0.0f, _cascadeDistribution - 0.05f);
            what = "distribution";
            break;
        case SDLK_B:
            _cascadeBlend = std::min(0.2f, _cascadeBlend + 0.01f);
            what = "blend";
            break;
        case SDLK_V:
            _cascadeBlend = std::max(0.0f, _cascadeBlend - 0.01f);
            what = "blend";
            break;
        case SDLK_EQUALS:
            _shadowResolution = std::min(2048, _shadowResolution * 2);
            what = "resolution";
            break;
        case SDLK_MINUS:
            _shadowResolution = std::max(128, _shadowResolution / 2);
            what = "resolution";
            break;
        case SDLK_T: {
            const auto it = std::find(kShadowTypes.begin(), kShadowTypes.end(), _shadowType);
            const size_t next = it == kShadowTypes.end()
                ? 0 : (static_cast<size_t>(it - kShadowTypes.begin()) + 1) % kShadowTypes.size();
            _shadowType = kShadowTypes[next];
            what = "type";
            break;
        }
        case SDLK_LEFTBRACKET:
            _vsmBlurSize = std::max(1, _vsmBlurSize - 2);
            what = "vsm blur";
            break;
        case SDLK_RIGHTBRACKET:
            _vsmBlurSize = std::min(25, _vsmBlurSize + 2);
            what = "vsm blur";
            break;
        case SDLK_SEMICOLON:
            _penumbraSize = std::max(0.0f, _penumbraSize - 0.005f);
            what = "penumbra";
            break;
        case SDLK_APOSTROPHE:
            _penumbraSize = std::min(0.2f, _penumbraSize + 0.005f);
            what = "penumbra";
            break;
        case SDLK_COMMA:
            _penumbraFalloff = std::max(1.0f, _penumbraFalloff - 0.5f);
            what = "falloff";
            break;
        case SDLK_PERIOD:
            _penumbraFalloff = std::min(10.0f, _penumbraFalloff + 0.5f);
            what = "falloff";
            break;
        default:
            return false;
        }

        applyLightSettings();
        logSettings(what);
        return true;
    }

private:
    void applyLightSettings() const
    {
        _light->setNumCascades(_numCascades);
        _light->setShadowResolution(_shadowResolution);
        _light->setCascadeDistribution(_cascadeDistribution);
        _light->setCascadeBlend(_cascadeBlend);
        _light->setShadowType(_shadowType);
        _light->setVsmBlurSize(_vsmBlurSize);
        _light->setPenumbraSize(_penumbraSize);
        _light->setPenumbraFalloff(_penumbraFalloff);
    }

    void logSettings(const char* reason) const
    {
        spdlog::info("CSM {}: cascades={}, resolution={}, distribution={:.2f}, blend={:.2f}, "
            "type={}, vsmBlur={}, penumbra={:.3f}, falloff={:.1f}",
            reason, _numCascades, _shadowResolution, _cascadeDistribution, _cascadeBlend,
            static_cast<int>(_shadowType), _vsmBlurSize, _penumbraSize, _penumbraFalloff);
    }

    static constexpr float kCloudSpeed = 0.2f;
    static constexpr std::array<ShadowType, 4> kShadowTypes = {
        SHADOW_PCF1_32F, SHADOW_PCF3_32F, SHADOW_VSM_16F, SHADOW_PCSS_32F
    };

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _terrainAsset;

    std::vector<Entity*> _clouds;
    float _time = 0.0f;

    LightComponent* _light = nullptr;
    CameraControls* _controls = nullptr;

    // Upstream's initial settings.light values.
    int _numCascades = 4;
    int _shadowResolution = 2048;
    float _cascadeDistribution = 0.5f;
    float _cascadeBlend = 0.1f;
    ShadowType _shadowType = SHADOW_PCF3_32F;
    int _vsmBlurSize = 11;
    float _penumbraSize = 0.02f;
    float _penumbraFalloff = 4.0f;
};

VISUTWIN_EXAMPLE_MAIN(ShadowCascadesExample)
