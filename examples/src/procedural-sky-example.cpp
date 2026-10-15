// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026
//
// Port of the upstream "procedural-sky" example.
//
// A laboratory bedded into dry-sand dunes, lit by a single sun that sweeps across a
// time-of-day cycle. The hour drives the sun's elevation and azimuth through smoothstep
// keyframe curves; the sun's elevation in turn drives the sky luminance, the bloom
// intensity, and the torches inside the laboratory, which glow only between sunset and
// sunrise. Shadows are PCSS over four cascades and re-render every frame, since the sun
// never stops moving. The sky is the ProceduralSky script: a Preetham daylight model and a
// night sky drawn per pixel, baked into the environment atlas that lights the scene, and
// steering the sun light.
//
// @credit Laboratory by Sketchfab, CC BY 4.0
// @credit FREE - Dry Sand Terrain by josevega, Sketchfab, CC BY 4.0
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "extras/script/cameraControls.h"
#include "extras/script/proceduralSky.h"
#include "../exampleApp.h"
#include "core/math/curve.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "framework/components/componentRegistry.h"

using namespace visutwin::canvas;

// Initial control values.
constexpr float INITIAL_HOUR = 9.0f;
constexpr float TIME_SPEED = 1.0f;      // hours per second
constexpr float SKY_EXPOSURE = 1.8f;
constexpr float SKY_TURBIDITY = 7.0f;
constexpr float SKY_RAYLEIGH = 1.0f;
constexpr float TORCH_INTENSITY = 60.0f;
constexpr float SUN_INTENSITY = 6.0f;    // the sun at full height

// Accumulated world-space bounds of every mesh instance under a node.
BoundingBox meshBounds(GraphNode* root)
{
    BoundingBox bounds;
    bool first = true;
    auto* rootEntity = dynamic_cast<Entity*>(root);
    if (!rootEntity) {
        return bounds;
    }
    for (auto* render : rootEntity->findComponents<RenderComponent>()) {
        auto* owner = render ? render->entity() : nullptr;
        if (!owner || (owner != root && !owner->isDescendantOf(root))) {
            continue;
        }
        for (auto* meshInstance : render->meshInstances()) {
            if (!meshInstance) continue;
            if (first) {
                bounds = meshInstance->aabb();
                first = false;
            } else {
                bounds.add(meshInstance->aabb());
            }
        }
    }
    return bounds;
}

Curve makeCurve(const std::vector<float>& keys)
{
    Curve curve(keys);
    curve.type = CURVE_SMOOTHSTEP;
    return curve;
}

class ProceduralSkyExample final: public ExampleApp
{
public:
    ProceduralSkyExample()
        : ExampleApp({.title = "Procedural Sky", .width = 1024, .height = 768}) {}

protected:
    bool create() override
    {
        scene()->setExposure(SKY_EXPOSURE);

        _laboratoryAsset = std::make_unique<Asset>(
            "laboratory", AssetType::CONTAINER, assetPath("models/laboratory.glb"));
        _terrainAsset = std::make_unique<Asset>(
            "terrain", AssetType::CONTAINER, assetPath("models/dry-sand-terrain.glb"));

        // ---------------------------------------------------------------------
        // Laboratory
        // ---------------------------------------------------------------------
        ContainerResource* labContainer = _laboratoryAsset->resourceAs<ContainerResource>();
        if (!labContainer) {
            spdlog::error("Failed to load models/laboratory.glb");
            return false;
        }
        auto* labEntity = labContainer->instantiateRenderEntity();
        labEntity->setEngine(engine());
        labEntity->setLocalScale(100.0f, 100.0f, 100.0f);
        root()->addChild(labEntity);

        // Materials use SSAO only — drop the baked AO map, and keep everything opaque.
        for (auto* render : engine()->components().instances<RenderComponent>()) {
            auto* owner = render ? render->entity() : nullptr;
            if (!owner || (owner != labEntity && !owner->isDescendantOf(labEntity))) {
                continue;
            }
            render->setCastShadows(true);
            render->setReceiveShadows(true);
            for (auto* meshInstance : render->meshInstances()) {
                if (auto* material = dynamic_cast<StandardMaterial*>(
                        meshInstance ? meshInstance->material() : nullptr)) {
                    material->setAoMap(nullptr);
                    material->setTransparent(false);
                    material->setBlendState(BlendState::noBlend());
                    material->setDepthState(std::make_shared<DepthState>());
                }
            }
        }

        // Torches: every node named 'Fackel*' gets a warm omni light whose intensity is
        // driven by the day/night cycle, so they only glow between sunset and sunrise.
        for (auto* torch : labEntity->find([](GraphNode* node) {
                 return node && node->name().find("Fackel") != std::string::npos;
             })) {
            // The mesh sits on a child node (the glTF splits node/primitive), so search the
            // subtree rather than the node itself.
            auto* torchEntity = dynamic_cast<Entity*>(torch);
            const auto renders = torchEntity ? torchEntity->findComponents<RenderComponent>()
                                             : std::vector<RenderComponent*>{};
            RenderComponent* render = nullptr;
            for (auto* candidate : renders) {
                if (candidate && !candidate->meshInstances().empty()) {
                    render = candidate;
                    break;
                }
            }
            if (!render) {
                continue;
            }
            auto* light = new Entity();
            light->setEngine(engine());
            if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
                lc->setType(LightType::LIGHTTYPE_OMNI);
                lc->setColor(Color(1.0f, 0.55f, 0.2f, 1.0f));
                lc->setIntensity(0.0f);
                lc->setRange(480.0f);
                lc->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
                _torchLights.push_back(lc);
            }
            // Place at the flame's world-space position.
            const auto centre = render->meshInstances()[0]->aabb().center();
            light->setLocalPosition(centre.getX(), centre.getY(), centre.getZ());
            root()->addChild(light);
        }
        spdlog::info("Torch lights found: {}", _torchLights.size());

        // ---------------------------------------------------------------------
        // Terrain — scaled to span the far clip and bedded under the laboratory
        // ---------------------------------------------------------------------
        ContainerResource* terrainContainer = _terrainAsset->resourceAs<ContainerResource>();
        if (!terrainContainer) {
            spdlog::error("Failed to load models/dry-sand-terrain.glb");
            return false;
        }
        auto* terrain = terrainContainer->instantiateRenderEntity();
        terrain->setEngine(engine());
        root()->addChild(terrain);

        const BoundingBox terrainAabb = meshBounds(terrain);
        constexpr float groundLevel = -40.0f;
        const float terrainScale = 3000.0f /
            (2.0f * std::max(terrainAabb.halfExtents().getX(), terrainAabb.halfExtents().getZ()));
        terrain->setLocalScale(terrainScale, terrainScale, terrainScale);

        const Vector3 tc = terrainAabb.center();
        const float terrainTop = (tc.getY() + terrainAabb.halfExtents().getY()) * terrainScale;
        terrain->setLocalPosition(
            -tc.getX() * terrainScale - 71.6f,
            groundLevel - terrainTop + 267.1f,
            -tc.getZ() * terrainScale + 395.8f);

        // Dim the bright sand by half so it balances against the darker building.
        std::vector<Material*> dimmed;
        for (auto* render : engine()->components().instances<RenderComponent>()) {
            auto* owner = render ? render->entity() : nullptr;
            if (!owner || (owner != terrain && !owner->isDescendantOf(terrain))) {
                continue;
            }
            render->setCastShadows(true);
            render->setReceiveShadows(true);
            for (auto* meshInstance : render->meshInstances()) {
                auto* material = dynamic_cast<StandardMaterial*>(
                    meshInstance ? meshInstance->material() : nullptr);
                if (!material || std::find(dimmed.begin(), dimmed.end(), material) != dimmed.end()) {
                    continue;
                }
                dimmed.push_back(material);
                const Color& d = material->diffuse();
                material->setDiffuse(Color(d.r * 0.5f, d.g * 0.5f, d.b * 0.5f, d.a));
            }
        }

        // ---------------------------------------------------------------------
        // Sun — PCSS soft shadows over four cascades, re-rendered every frame
        // ---------------------------------------------------------------------
        _sunEntity = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), SUN_INTENSITY, true);
        if (auto* sun = _sunEntity->findComponent<LightComponent>()) {
            sun->setShadowType(ShadowType::SHADOW_PCSS_32F);
            sun->setPenumbraSize(0.03f);
            sun->setPenumbraFalloff(2.1f);
            sun->setShadowResolution(2048);
            sun->setNumCascades(4);
            sun->setCascadeDistribution(0.35f);
            sun->setShadowBias(0.18f);
            sun->setShadowNormalBias(0.82f);
            sun->setShadowDistance(2400.0f);
        }

        // ---------------------------------------------------------------------
        // Procedural sky — draws the sky, lights the scene from it, steers the sun light
        // ---------------------------------------------------------------------
        auto* skyEntity = new Entity();
        skyEntity->setName("ProceduralSky");
        skyEntity->setEngine(engine());
        skyEntity->addComponent<ScriptComponent>();
        _sky = skyEntity->script()->create<ProceduralSky>();
        if (_sky) {
            _sky->setSunLight(_sunEntity);
            _sky->setTurbidity(SKY_TURBIDITY);
            _sky->setRayleigh(SKY_RAYLEIGH);
        }
        root()->addChild(skyEntity);

        // ---------------------------------------------------------------------
        // Camera — wide angle so more of the sky is visible
        // ---------------------------------------------------------------------
        auto* cameraEntity = createCamera(Vector3(240.0f, 85.0f, 240.0f));
        _cameraComponent = cameraEntity->findComponent<CameraComponent>();

        if (_cameraComponent) {
            if (auto* camera = _cameraComponent->camera()) {
                camera->setFov(80.0f);
                camera->setFarClip(3000.0f);
            }
            _cameraComponent->setToneMapping(TONEMAP_NEUTRAL);

            // SSAO applied to the ambient lighting rather than as a post-process.
            auto ssao = _cameraComponent->ssao();
            ssao.enabled = true;
            ssao.type = "lighting";
            ssao.blurEnabled = true;
            ssao.intensity = 0.4f;
            ssao.power = 6.0f;
            ssao.radius = 30.0f;
            ssao.samples = 12;
            ssao.minAngle = 10.0f;
            _cameraComponent->setSsao(ssao);

            auto rendering = _cameraComponent->rendering();
            rendering.bloomIntensity = 0.0f;   // driven by the sun elevation below
            rendering.bloomBlurLevel = 16;
            _cameraComponent->setRendering(rendering);
        }

        auto* cameraControls = addOrbitControls(cameraEntity, Vector3(0.0f, 25.0f, 0.0f));
        cameraControls->setZoomRange(Vector2(1.0f, 500.0f));
        cameraControls->storeResetState();

        spdlog::info("Procedural sky: Space pauses the day cycle, R resets the camera, Esc quits.");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
            _animate = !_animate;
            return true;
        }
        return false;
    }

    void update(const float dt) override
    {
        // Advance the time of day, skipping the (boring) night by jumping 20:00 -> 05:00.
        if (_animate) {
            _hour += dt * TIME_SPEED;
            if (_hour >= 20.0f) {
                _hour -= 15.0f;
            }
        }

        // Sun: elevation from the time curve, azimuth sweeping east -> west across the day;
        // the sky's luminance follows the elevation.
        const float elevation = _elevationCurve.value(_hour);
        if (_sky) {
            _sky->setElevation(elevation);
            _sky->setAzimuth((_hour / 24.0f) * 360.0f);
            _sky->setLuminance(_luminanceCurve.value(elevation));
        }

        // Bloom intensity is driven by the sun elevation.
        if (_cameraComponent) {
            const float bloom = _bloomCurve.value(elevation);
            if (bloom != _lastBloom) {
                auto rendering = _cameraComponent->rendering();
                rendering.bloomIntensity = bloom;
                _cameraComponent->setRendering(rendering);
                _lastBloom = bloom;
            }
        }

        // Torches glow between sunset and sunrise.
        const float t = std::clamp((elevation + 3.0f) / 6.0f, 0.0f, 1.0f);
        const float nightFactor = 1.0f - (t * t * (3.0f - 2.0f * t));   // 1 - smoothstep(-3, 3, elevation)
        for (auto* torch : _torchLights) {
            torch->setIntensity(TORCH_INTENSITY * nightFactor);
        }
    }

private:
    std::unique_ptr<Asset> _laboratoryAsset;
    std::unique_ptr<Asset> _terrainAsset;

    std::vector<LightComponent*> _torchLights;
    Entity* _sunEntity = nullptr;
    ProceduralSky* _sky = nullptr;
    CameraComponent* _cameraComponent = nullptr;

    // Time-of-day curves (keyframes, smoothstepped)
    Curve _elevationCurve = makeCurve({0.0f, -60.0f, 6.0f, 0.0f, 12.0f, 60.0f, 18.0f, 0.0f, 24.0f, -90.0f});
    Curve _luminanceCurve = makeCurve({0.0f, 2.0f, 35.0f, 0.4f, 90.0f, 0.3f});
    Curve _bloomCurve     = makeCurve({0.0f, 0.005f, 5.0f, 0.001f, 8.0f, 0.001f, 90.0f, 0.002f});

    float _hour = INITIAL_HOUR;
    bool _animate = true;
    float _lastBloom = -1.0f;
};

VISUTWIN_EXAMPLE_MAIN(ProceduralSkyExample)
