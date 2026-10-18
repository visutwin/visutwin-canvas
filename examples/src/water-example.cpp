// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// Port of upstream graphics/water.
//
// A configurable water surface rendered by the Water script around two copies of the tropical
// island (the second turned 140 degrees and sunk a little, so mostly its rocks break the
// surface): a planar reflection and refraction, absorption with depth, shoreline foam, Gerstner
// waves, caustics on the island below the water and an underwater view. The water is a circle
// of radius 1200 (180 sectors, 150 rings gathered toward the centre by an exponent of 2.6) on a
// layer of its own after the world's transparent sublayer, at the island's sea level of -0.62.
// The island's materials are dimmed to 0.55 (their textures carry baked lighting) and take the
// water's underwater fog and caustics; the scene fog is EXP at 0.068, its colour following the
// daylight. The ProceduralSky script lights the scene and steers the sun (intensity 1.3, shadow
// bias 0.2, normal offset 0.05, distance 120) by the time of day, 10:00, through upstream's
// elevation, luminance and bloom curves; neutral tone mapping with bloom over 16 levels. The
// camera (fov 60, far 3000) orbits a point between the islands and may dive below the surface.
//
// Keys stand in for upstream's controls panel:
//   [ / ]  the time of day -/+ 0.5 h
//   P      the water preset, cycled tropical / ocean / murky
//   R      the reflection, cycled planar / sky / none
//   F      refraction on / off
//   D      depth effects on / off
//   O      foam on / off
//   C      caustics on / off
//   W      waves on / off
// VISUTWIN_WATER_HOUR=h starts at that hour.
//
// DEVIATIONS:
// - the island's textures are the same pixels re-encoded from WebP to PNG
//   (assets/models/tropical-island.credits.txt), which the image decoder here reads.
// - upstream's CameraControls focus point and ranges are kept, through the examples' orbit
//   controls.
//
// @credit Tropical Island by Elin, Sketchfab, CC BY 4.0
// @credit Caustic Lighting by Davis3D, OpenGameArt, CC0
//
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <set>
#include <vector>

#include "core/math/curve.h"
#include "extras/script/cameraControls.h"
#include "extras/script/proceduralSky.h"
#include "extras/script/water.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/render/primitiveGeometry.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/composition/layerComposition.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The layer of the water, which its internal cameras do not render.
    constexpr int LAYERID_WATER = 100;

    struct WaterPreset
    {
        const char* name;
        Color shallowColor;
        Color deepColor;
        float depthFade;
    };

    const WaterPreset kPresets[] = {
        {"tropical", Color(0.6f, 0.9f, 0.85f, 1.0f), Color(0.03f, 0.16f, 0.24f, 1.0f), 3.0f},
        {"ocean", Color(0.35f, 0.55f, 0.6f, 1.0f), Color(0.01f, 0.06f, 0.12f, 1.0f), 6.0f},
        {"murky", Color(0.5f, 0.55f, 0.35f, 1.0f), Color(0.1f, 0.14f, 0.05f, 1.0f), 1.2f},
    };

    Curve smoothstepCurve(const std::vector<float>& keys)
    {
        Curve curve(keys);
        curve.type = CURVE_SMOOTHSTEP;
        return curve;
    }
}

class WaterExample final: public ExampleApp
{
public:
    WaterExample(): ExampleApp({.title = "Water"}) {}

protected:
    bool create() override
    {
        _island = std::make_unique<Asset>("island", AssetType::CONTAINER, assetPath("models/tropical-island.glb"));
        _normal = std::make_unique<Asset>("normal", AssetType::TEXTURE, assetPath("textures/normal-map.png"));
        _caustics = std::make_unique<Asset>("caustics", AssetType::TEXTURE, assetPath("textures/caustics.jpg"));
        ContainerResource* container = _island->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Failed to load the island");
            return false;
        }

        // A layer for the water plane, after the world's transparent sublayer, so the internal
        // cameras can leave it out.
        const auto layers = scene()->layers();
        const auto worldLayer = layers->getLayerById(LAYERID_WORLD);
        auto waterLayer = std::make_shared<Layer>("Water", LAYERID_WATER);
        layers->insert(waterLayer, layers->getTransparentIndex(worldLayer) + 1);

        // The tropical island, its terrain continuing under the water.
        Entity* island = container->instantiateRenderEntity();
        root()->addChild(island);

        // A second copy close by, turned and sunk a bit deeper, so mostly the rocks break the
        // surface; a wrapper carries the transform, the instance root has the model's own.
        auto* island2 = new Entity();
        island2->setName("Island2");
        island2->setEngine(engine());
        island2->addChild(container->instantiateRenderEntity());
        island2->setLocalPosition(15.5f, -0.59f, -10.9f);
        island2->setLocalEulerAngles(0.0f, 140.0f, 0.0f);
        root()->addChild(island2);
        for (Entity* entity : {island, island2}) {
            for (auto* render : entity->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
            }
        }

        // The island's textures have lighting baked in, so their albedo is dimmed against the
        // sun and sky on top (both copies share the materials). They take the water's underwater
        // effects too.
        for (auto* render : island->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                if (auto* material = dynamic_cast<StandardMaterial*>(meshInstance->material())) {
                    _islandMaterials.insert(material);
                }
            }
        }
        for (StandardMaterial* material : _islandMaterials) {
            material->setDiffuse(Color(0.55f, 0.55f, 0.55f, 1.0f));
        }

        // The underwater fog, which the water limits to the fragments below its level.
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_EXP);

        // The sun, lighting the scene and the water; the sky steers it by the time of day.
        _sun = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.3f, true);
        _sun->setName("Sun");
        if (auto* light = _sun->findComponent<LightComponent>()) {
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.05f);
            light->setShadowDistance(120.0f);
        }

        // The procedural sky: the visible sky and the image-based lighting.
        auto* skyEntity = new Entity();
        skyEntity->setName("ProceduralSky");
        skyEntity->setEngine(engine());
        skyEntity->addComponent<ScriptComponent>();
        _sky = skyEntity->script()->create<ProceduralSky>();
        _sky->setSunLight(_sun);
        _sky->setMoonIntensity(0.4f);
        root()->addChild(skyEntity);

        // The camera, rendering the water layer too; its frame tone maps and blooms.
        _camera = createCamera(Vector3(35.0f, 13.0f, 42.0f));
        _camera->setName("MainCamera");
        _cameraComp = _camera->findComponent<CameraComponent>();
        _cameraComp->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX, LAYERID_WATER, LAYERID_UI});
        _cameraComp->camera()->setFov(60.0f);
        _cameraComp->camera()->setFarClip(3000.0f);
        _cameraComp->setToneMapping(TONEMAP_NEUTRAL);
        auto rendering = _cameraComp->rendering();
        rendering.bloomBlurLevel = 16;
        _cameraComp->setRendering(rendering);
        // Orbiting the point between the islands, diving below the water allowed.
        if (auto* controls = addOrbitControls(_camera, Vector3(8.0f, 0.0f, -5.0f))) {
            controls->setPitchRange(Vector2(-90.0f, 89.0f));
            controls->setZoomRange(Vector2(2.0f, 150.0f));
            controls->storeResetState();
        }

        // The water: a tessellated circle, so the waves can displace it, its tessellation
        // gathered near the centre where the camera is.
        _waterMesh = createMeshFromGeometry(device(), createCircleGeometry(1200.0f, 180, 150, 2.6f));
        _waterPlaceholder = std::make_shared<StandardMaterial>();
        auto* water = new Entity();
        water->setName("Water");
        water->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(water->addComponent<RenderComponent>())) {
            render->setLayers({LAYERID_WATER});
            render->setCastShadows(false);
            auto meshInstance = std::make_unique<MeshInstance>(_waterMesh.get(), _waterPlaceholder.get(), water);
            meshInstance->setCastShadow(false);
            render->addMeshInstance(std::move(meshInstance));
        }
        // The island model's sea level.
        water->setLocalPosition(0.0f, -0.62f, 0.0f);
        root()->addChild(water);

        water->addComponent<ScriptComponent>();
        _water = water->script()->create<Water>();
        _water->setCameraEntity(_camera);
        _water->setLightEntity(_sun);
        _water->normalMap = _normal->resourceAs<Texture>();
        _water->causticsMap = _caustics->resourceAs<Texture>();

        // Upstream's initial control values.
        _water->reflectionStrength = 1.0f;
        _water->skyBlur = 0.0f;
        _water->distortion = 0.03f;
        _water->opacity = 0.8f;
        _water->clipBias = 0.1f;
        _water->textureScale = 0.5f;
        _water->shoreSoftness = 0.3f;
        _water->bumpiness = 0.4f;
        _water->rippleTiling = 0.08f;
        _water->rippleSpeed = 0.05f;
        _water->fresnelPower = 5.0f;
        _water->specularPower = 256.0f;
        _water->specularIntensity = 1.0f;
        _water->diffuseIntensity = 0.35f;
        _water->foamDepth = 0.5f;
        _water->causticsStrength = 4.0f;
        _water->causticsTiling = 0.1f;
        _water->causticsSpeed = 0.04f;
        _water->causticsDepth = 4.9f;
        _water->snellWindow = 0.6f;
        _water->waves = true;
        _water->waveAmplitude = 0.12f;
        _water->waveLength = 12.0f;
        _water->waveSpeed = 1.0f;
        _water->waveSteepness = 0.4f;
        _water->waveDirection = 0.0f;
        _water->swellAmplitude = 0.25f;
        _water->swellLength = 35.0f;
        _water->swellSpeed = 1.0f;
        _water->swellDirection = 40.0f;
        scene()->setFogDensity(0.068f);

        if (const char* hour = std::getenv("VISUTWIN_WATER_HOUR")) {
            _hour = static_cast<float>(std::atof(hour));
        }
        applyPreset();
        applyCaustics();
        applyTimeOfDay();

        spdlog::info("Controls: [ ] hour, P preset, R reflection, F refraction, D depth effects, O foam, "
                     "C caustics, W waves");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_water) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_LEFTBRACKET: _hour = std::fmod(_hour + 23.5f, 24.0f); applyTimeOfDay(); break;
        case SDLK_RIGHTBRACKET: _hour = std::fmod(_hour + 0.5f, 24.0f); applyTimeOfDay(); break;
        case SDLK_P: _preset = (_preset + 1) % std::size(kPresets); applyPreset(); break;
        case SDLK_R:
            _water->reflectionSource = _water->reflectionSource == WaterReflection::Planar ? WaterReflection::Sky
                : _water->reflectionSource == WaterReflection::Sky ? WaterReflection::None : WaterReflection::Planar;
            break;
        case SDLK_F: _water->refraction = !_water->refraction; break;
        case SDLK_D: _water->depthEffects = !_water->depthEffects; break;
        case SDLK_O: _water->foam = !_water->foam; break;
        case SDLK_C: _causticsOn = !_causticsOn; applyCaustics(); break;
        case SDLK_W: _water->waves = !_water->waves; break;
        default: return false;
        }
        spdlog::info("hour {:.1f}, preset {}, reflection {}, refraction {}, depth effects {}, foam {}, caustics {}, "
                     "waves {}", _hour, kPresets[_preset].name,
            _water->reflectionSource == WaterReflection::Planar ? "planar"
                : _water->reflectionSource == WaterReflection::Sky ? "sky" : "none",
            _water->refraction, _water->depthEffects, _water->foam, _causticsOn, _water->waves);
        return true;
    }

private:
    // The sun's position, the sky's luminance, the bloom and the fog's daylight for the hour.
    void applyTimeOfDay()
    {
        const float elevation = _elevationCurve.value(_hour);
        _sky->setElevation(elevation);
        _sky->setAzimuth(_hour / 24.0f * 360.0f);
        _sky->setLuminance(_luminanceCurve.value(std::max(elevation, 0.0f)));

        // The underwater fog dims with the daylight, with a small floor for the moonlit night.
        const float t = std::clamp((elevation + 2.0f) / 14.0f, 0.0f, 1.0f);
        _fogDayFactor = 0.05f + 0.95f * t * t * (3.0f - 2.0f * t);
        applyFogColor();

        auto rendering = _cameraComp->rendering();
        rendering.bloomIntensity = _bloomCurve.value(std::max(elevation, 0.0f));
        _cameraComp->setRendering(rendering);
    }

    void applyFogColor()
    {
        scene()->setFogColor(Color(_fogColor[0] * _fogDayFactor, _fogColor[1] * _fogDayFactor,
            _fogColor[2] * _fogDayFactor, 1.0f));
    }

    // The water colours of a preset. Upstream also derives a fog colour and density from it,
    // then restores the tuned initial values, so the initial preset leaves the fog as set.
    void applyPreset()
    {
        const WaterPreset& preset = kPresets[_preset];
        _water->shallowColor = preset.shallowColor;
        _water->deepColor = preset.deepColor;
        _water->depthFade = preset.depthFade;
        if (_presetApplied) {
            // A brighter blueish fog between the preset's deep and shallow colours.
            const float brightness = 0.35f;
            _fogColor[0] = preset.deepColor.r + (preset.shallowColor.r - preset.deepColor.r) * brightness;
            _fogColor[1] = preset.deepColor.g + (preset.shallowColor.g - preset.deepColor.g) * brightness;
            _fogColor[2] = preset.deepColor.b + (preset.shallowColor.b - preset.deepColor.b) * brightness;
            scene()->setFogDensity(0.45f / preset.depthFade);
            applyFogColor();
        }
        _presetApplied = true;
    }

    void applyCaustics()
    {
        for (StandardMaterial* material : _islandMaterials) {
            Water::applyUnderwaterEffects(*material, true, _causticsOn);
        }
    }

    std::unique_ptr<Asset> _island;
    std::unique_ptr<Asset> _normal;
    std::unique_ptr<Asset> _caustics;
    std::set<StandardMaterial*> _islandMaterials;
    std::shared_ptr<Mesh> _waterMesh;
    std::shared_ptr<StandardMaterial> _waterPlaceholder;
    Entity* _sun = nullptr;
    Entity* _camera = nullptr;
    CameraComponent* _cameraComp = nullptr;
    ProceduralSky* _sky = nullptr;
    Water* _water = nullptr;

    // The sun's elevation over the day (hour, degrees), the sky's luminance and the bloom by the
    // sun's elevation.
    Curve _elevationCurve = smoothstepCurve({0.0f, -60.0f, 6.0f, 0.0f, 12.0f, 60.0f, 18.0f, 0.0f, 24.0f, -90.0f});
    Curve _luminanceCurve = smoothstepCurve({0.0f, 2.0f, 35.0f, 0.4f, 90.0f, 0.3f});
    Curve _bloomCurve = smoothstepCurve({0.0f, 0.005f, 5.0f, 0.001f, 8.0f, 0.001f, 90.0f, 0.002f});

    float _hour = 10.0f;
    size_t _preset = 0;
    bool _presetApplied = false;
    bool _causticsOn = true;
    float _fogColor[3] = {0.165f, 0.549f, 0.596f};
    float _fogDayFactor = 1.0f;
};

VISUTWIN_EXAMPLE_MAIN(WaterExample)
