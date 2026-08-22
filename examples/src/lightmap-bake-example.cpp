// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream's graphics/lights-baked-a-o: the house.glb scene
// — which ships with a generated UV1 channel unwrapped for lightmapping and
// stripped textures — is lit by a directional sun and baked (on the CPU) into
// per-mesh lightmaps with hard shadows + ambient occlusion. The whole house
// geometry is registered as ray occluders so submeshes shadow each other and
// AO forms in the crevices. The helipad env atlas provides the skybox / ambient.
// Each house mesh is masked out of realtime lighting (MASK_AFFECT_LIGHTMAPPED),
// so its whole look comes from the bake. Auto-toggles the lightmaps ON/OFF every
// 3 s to show the baked shadows + AO appear/disappear. Esc quits.
//
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "core/math/quaternion.h"
#include "core/shape/boundingBox.h"
#include "framework/assets/asset.h"
#include "framework/lightmapper/gpuLightmapper.h"
#include "framework/lightmapper/lightmapper.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

const Color AMBIENT_LIGHT(0.1f, 0.3f, 0.4f, 1.0f);

// A light entity's euler angles to the world direction it emits along. Lights point
// down their node's -Y axis (the same convention the spot shadow camera corrects for).
Vector3 eulerToDirection(const float x, const float y, const float z)
{
    return (Quaternion::fromEulerAngles(x, y, z) * Vector3(0.0f, -1.0f, 0.0f)).normalized();
}

// Gather every mesh instance under `entity` (all render components in the subtree).
std::vector<MeshInstance*> collectMeshInstances(Entity* entity)
{
    std::vector<MeshInstance*> out;
    for (auto* render : RenderComponent::instances()) {
        if (!render || !render->entity()) {
            continue;
        }
        auto* owner = render->entity();
        if (owner != entity && !owner->isDescendantOf(entity)) {
            continue;
        }
        for (auto* mi : render->meshInstances()) {
            if (mi && mi->mesh()) {
                out.push_back(mi);
            }
        }
    }
    return out;
}

class LightmapBakeExample final: public ExampleApp
{
public:
    LightmapBakeExample()
        : ExampleApp({.title = "Lightmap Bake"}) {}

protected:
    bool create() override
    {
        scene()->setSkyboxMip(3);
        scene()->setSkyboxIntensity(0.6f);
        scene()->setAmbientLight(AMBIENT_LIGHT.r, AMBIENT_LIGHT.g, AMBIENT_LIGHT.b);

        // Helipad environment atlas: skybox + ambient (main source of ambient light).
        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{ .type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false }
        );
        // House scene: has a generated UV1 lightmap channel + stripped textures.
        _house = std::make_unique<Asset>(
            "house", AssetType::CONTAINER, assetPath("models/house.glb"));

        const auto helipadResource = _helipad->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad env atlas");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        // Instantiate the house (unwrapped UV1 for lightmapping), scaled up like upstream.
        const auto houseResource = _house->resource();
        if (!houseResource) {
            spdlog::error("Failed to load house model");
            return false;
        }
        auto* houseEntity = std::get<ContainerResource*>(*houseResource)->instantiateRenderEntity();
        houseEntity->setLocalScale(100.0f, 100.0f, 100.0f);
        root()->addChild(houseEntity);

        _houseBounds = entityBounds(houseEntity);

        // Every renderable mesh of the house.
        _houseMeshes = collectMeshInstances(houseEntity);
        spdlog::info("House has {} mesh instance(s)", _houseMeshes.size());

        // ── Bake the house lightmaps ─────────────────────────────────────────────
        _lightmapper = std::make_unique<Lightmapper>(device().get());

        // JS: lightDirectional — euler (-55, 0, -30), color (0.7,0.7,0.5), intensity 1.6,
        //     bake with bakeNumSamples 15 over a bakeArea of 10 degrees (soft shadows).
        // Upstream creates these as real light entities with affectDynamic:false /
        // affectLightmapped:true / bake:true. The equivalent here is MASK_BAKE, which keeps
        // them out of realtime lighting while the GPU bake (which renders the mesh under
        // MASK_BAKE) still sees them. The CPU baker takes its own copies of the same values.
        _directionalLight = addBakeLight("Directional", LightType::LIGHTTYPE_DIRECTIONAL,
            Color(0.7f, 0.7f, 0.5f), 1.6f, Vector3(0.0f, 0.0f, 0.0f), Vector3(-55.0f, 0.0f, -30.0f), 0.0f);
        _directionalLight->setShadowDistance(100.0f);
        _omniLight = addBakeLight("Omni", LightType::LIGHTTYPE_OMNI, Color(1.0f, 1.0f, 0.0f),
            0.9f, Vector3(-4.0f, 10.0f, 5.0f), Vector3(0.0f, 0.0f, 0.0f), 25.0f);
        _spotLight = addBakeLight("Spot", LightType::LIGHTTYPE_SPOT, Color(1.0f, 0.0f, 0.0f),
            2.5f, Vector3(-5.0f, 10.0f, -7.5f), Vector3(0.0f, 0.0f, 0.0f), 10.0f);

        _gpuBakeStart = std::chrono::steady_clock::now();

        // GPU baker (upstream's UV-space render). Needs the lights to exist as real scene
        // LightComponents, since the bake evaluates the ordinary lit pipeline; the CPU baker
        // instead takes its own light descriptions below.
        _gpuLightmapper = std::make_unique<GpuLightmapper>(engine());

        _sun.type = LightType::LIGHTTYPE_DIRECTIONAL;
        _sun.direction = eulerToDirection(-55.0f, 0.0f, -30.0f);
        _sun.color = Color(0.7f, 0.7f, 0.5f, 1.0f);
        _sun.intensity = 1.6f;
        _sun.castShadows = true;
        _sun.bakeNumSamples = 15;
        _sun.bakeArea = 10.0f;

        // JS: lightOmni — yellow, position (-4, 10, 5), range 25, intensity 0.9, bake: true
        _omni.type = LightType::LIGHTTYPE_OMNI;
        _omni.position = Vector3(-4.0f, 10.0f, 5.0f);
        _omni.color = Color(1.0f, 1.0f, 0.0f, 1.0f);
        _omni.intensity = 0.9f;
        _omni.range = 25.0f;
        _omni.castShadows = true;

        // JS: lightSpot — red, position (-5, 10, -7.5), range 10, intensity 2.5, bake: true.
        // Upstream leaves the spot at its default rotation, which points straight down.
        _spot.type = LightType::LIGHTTYPE_SPOT;
        _spot.position = Vector3(-5.0f, 10.0f, -7.5f);
        _spot.direction = Vector3(0.0f, -1.0f, 0.0f);
        _spot.color = Color(1.0f, 0.0f, 0.0f, 1.0f);
        _spot.intensity = 2.5f;
        _spot.range = 10.0f;
        _spot.castShadows = true;

        // JS: app.scene.lightmapMode = BAKE_COLOR; lightmapMaxResolution = 1024;
        //     lightmapSizeMultiplier = 512; plus the HUD defaults for ambient + filter.
        _bakeOptions.sizeMultiplier = 512.0f;
        _bakeOptions.maxResolution = 1024;
        _bakeOptions.ambient = AMBIENT_LIGHT;               // scene ambient, baked in
        _bakeOptions.skyColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        _bakeOptions.ambientBake = true;
        _bakeOptions.ambientBakeNumSamples = 20;
        _bakeOptions.ambientBakeSpherePart = 0.4f;          // HUD "hemisphere" on
        _bakeOptions.ambientBakeOcclusionContrast = -0.6f;
        _bakeOptions.ambientBakeOcclusionBrightness = -0.5f;
        _bakeOptions.aoRadius = 40.0f;                      // occlusion ray reach, world units
        _bakeOptions.filterEnabled = true;
        _bakeOptions.filterRange = 10.0f;
        _bakeOptions.filterSmoothness = 0.2f;
        _bakeOptions.dilatePixels = 6;

        spdlog::info("Baking house lightmaps on the GPU ({} meshes)...", _houseMeshes.size());
        gpuRebake();

        // JS: camera clearColor (0.4, 0.45, 0.5), farClip 100, nearClip 1, position (40, 20, 40),
        //     orbiting the house. DEVIATION: upstream's orbit-camera script becomes lookAt here.
        auto* camera = createCamera(Vector3(40.0f, 20.0f, 40.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>();
            cameraComponent && cameraComponent->camera()) {
            cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
            cameraComponent->camera()->setNearClip(1.0f);
            cameraComponent->camera()->setFarClip(100.0f);
        }

        // JS: camera.script.create('orbitCamera', { inertiaFactor: 0.2, focusEntity: house,
        //     distanceMax: 60 }) + orbitCameraInputMouse/Touch — drag to orbit, wheel to zoom.
        camera->lookAt(_houseBounds.center());
        if (auto* cameraControls = addOrbitControls(camera, _houseBounds.center())) {
            cameraControls->setZoomRange(Vector2(5.0f, 60.0f));   // JS: distanceMax 60
        }

        // Upstream's HUD panels become keys; each one re-bakes like the HUD does.
        spdlog::info("Keys: 1 directional  2 other lights (GPU re-bake)  |  "
                     "3 ambient bake  4 hemisphere  5 lightmap filter (CPU re-bake)  |  "
                     "G GPU bake  C CPU ray-traced bake  L show/hide  |  ESC quits");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }

        bool bakeSettingChanged = true;
        switch (event.key.key) {
        case SDLK_1:
            _directionalEnabled = !_directionalEnabled;
            spdlog::info("Directional light: {}", _directionalEnabled ? "on" : "off");
            break;
        case SDLK_2:
            _otherLightsEnabled = !_otherLightsEnabled;
            spdlog::info("Other lights (omni + spot): {}", _otherLightsEnabled ? "on" : "off");
            break;
        case SDLK_3:
            _ambientBakeEnabled = !_ambientBakeEnabled;
            spdlog::info("Ambient bake: {}", _ambientBakeEnabled ? "on" : "off");
            break;
        case SDLK_4:
            _hemisphereEnabled = !_hemisphereEnabled;
            spdlog::info("Ambient hemisphere: {}", _hemisphereEnabled ? "on (0.4)" : "off (full sphere)");
            break;
        case SDLK_5:
            _filterEnabled = !_filterEnabled;
            spdlog::info("Lightmap filter: {}", _filterEnabled ? "on" : "off");
            break;
        case SDLK_G:
            gpuRebake();
            return true;
        case SDLK_C:
            spdlog::info("CPU ray-traced re-bake...");
            rebake();
            _lightmapOn = true;
            return true;
        case SDLK_L:
            _lightmapOn = !_lightmapOn;
            for (auto& [mat, tex] : _bakedMaterials) {
                mat->setLightMap(_lightmapOn ? tex.get() : nullptr);
            }
            spdlog::info("House lightmaps: {}", _lightmapOn ? "ON (baked shadows + AO)" : "OFF");
            return true;
        default:
            bakeSettingChanged = false;
            break;
        }

        if (bakeSettingChanged) {
            // Lights affect both bakers; the CPU-only quality knobs (3/4/5) still
            // need the ray-traced path, so those re-bake on the CPU.
            if (event.key.key == SDLK_1 || event.key.key == SDLK_2 ||
                event.key.key == SDLK_3 || event.key.key == SDLK_4) {
                gpuRebake();
            } else {
                rebake();
                _lightmapOn = true;
            }
            return true;
        }
        return false;
    }

    void postRender() override
    {
        // Collect a GPU bake once its frame has rendered (same pattern as ReflectionProbe).
        if (_gpuLightmapper->baking() && _gpuLightmapper->update()) {
            const auto gpuMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - _gpuBakeStart).count();
            spdlog::info("GPU bake duration: {} ms", gpuMs);
            _lightmapOn = true;
        }
    }

    void destroy() override
    {
        // Both hold engine/device resources.
        _gpuLightmapper.reset();
        _lightmapper.reset();
    }

private:
    LightComponent* addBakeLight(const char* name, const LightType type, const Color& color,
        const float intensity, const Vector3& position, const Vector3& eulerAngles,
        const float range) const
    {
        auto* entity = new Entity();
        entity->setName(name);
        entity->setEngine(engine());
        root()->addChild(entity);
        entity->setLocalPosition(position);
        entity->setLocalEulerAngles(eulerAngles.getX(), eulerAngles.getY(), eulerAngles.getZ());

        auto* light = static_cast<LightComponent*>(entity->addComponent<LightComponent>());
        light->setType(type);
        light->setColor(color);
        light->setIntensity(intensity);
        light->setCastShadows(true);
        // DEVIATION: upstream's shadowBias 0.2 is in its own units; here setShadowBias takes
        // the 0..1 authoring value and the pass applies bias * -1000 as a polygon offset, so
        // 0.2 would push casters ~4x further than the engine's usual 0.05 and erase the
        // small ones (the tree and the car lost their shadows entirely).
        light->setShadowBias(0.05f);
        light->setShadowNormalBias(0.05f);   // JS: normalOffsetBias 0.05
        light->setShadowResolution(2048);    // JS: shadowResolution 2048
        light->setMask(MASK_BAKE);
        if (range > 0.0f) {
            light->setRange(range);
        }
        return light;
    }

    // Re-bake with the current flags. Upstream re-bakes whenever a HUD setting
    // changes and reports the duration in its "Bake stats" panel.
    void rebake()
    {
        _lightmapper->clear();
        if (_directionalEnabled) {
            _lightmapper->addLight(_sun);
        }
        if (_otherLightsEnabled) {
            _lightmapper->addLight(_omni);
            _lightmapper->addLight(_spot);
        }
        for (auto* mi : _houseMeshes) {
            if (auto* node = mi->node()) {
                _lightmapper->addOccluder(*mi->mesh(), node->worldTransform());
            }
        }

        _bakeOptions.ambientBake = _ambientBakeEnabled;
        _bakeOptions.ambientBakeSpherePart = _hemisphereEnabled ? 0.4f : 1.0f;
        _bakeOptions.filterEnabled = _filterEnabled;

        _bakedMaterials.clear();
        _seenMaterials.clear();
        _keepAlive.clear();

        const auto bakeStart = std::chrono::steady_clock::now();
        for (auto* mi : _houseMeshes) {
            auto* node = mi->node();
            if (!node) {
                continue;
            }
            auto lightmap = _lightmapper->bake(*mi->mesh(), node->worldTransform(), _bakeOptions);
            if (!lightmap) {
                continue;
            }
            _keepAlive.push_back(lightmap);
            mi->setMask(MASK_AFFECT_LIGHTMAPPED);

            if (auto* stdMat = dynamic_cast<StandardMaterial*>(mi->material())) {
                stdMat->setLightMap(lightmap.get());
                // DEVIATION/RISK: submeshes sharing one StandardMaterial but with
                // distinct UV1 layouts collide — last bake wins. Track the material
                // once so the toggle stays consistent.
                if (_seenMaterials.insert(stdMat).second) {
                    _bakedMaterials.emplace_back(stdMat, std::move(lightmap));
                } else {
                    _bakedMaterials.back().second = _keepAlive.back();
                }
            }
        }
        const auto bakeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - bakeStart).count();
        // JS: data.stats.duration
        spdlog::info("Bake duration: {} ms ({} lightmapped material(s))", bakeMs, _bakedMaterials.size());
    }

    // The GPU bake is upstream's own mechanism and costs a single frame, so it is what
    // the example starts with; the CPU ray-traced bake (rebake(), key C) stays available
    // as the higher-quality reference — true AO and soft shadows for seconds of work.
    void gpuRebake()
    {
        _directionalLight->setEnabled(_directionalEnabled);
        _omniLight->setEnabled(_otherLightsEnabled);
        _spotLight->setEnabled(_otherLightsEnabled);

        GpuLightmapper::Options gpuOptions;
        gpuOptions.sizeMultiplier = 512.0f;    // JS: app.scene.lightmapSizeMultiplier
        gpuOptions.maxResolution = 1024;       // JS: app.scene.lightmapMaxResolution
        // JS: ambientBake with ambientBakeNumSamples 20 over spherePart 0.4 — here those
        // become virtual directional lights accumulated into the lightmap.
        // JS: the directional light bakes with bakeNumSamples 15 over a bakeArea of 10
        // degrees — soft-edged shadows rather than one hard shadow map.
        gpuOptions.directionalBakeNumSamples = 15;
        gpuOptions.directionalBakeArea = 10.0f;
        gpuOptions.ambientBake = _ambientBakeEnabled;
        gpuOptions.ambientBakeNumSamples = 20;
        gpuOptions.ambientBakeSpherePart = _hemisphereEnabled ? 0.4f : 1.0f;
        gpuOptions.bakeCameraTarget = _houseBounds.center();
        gpuOptions.bakeCameraDistance = std::max(_houseBounds.halfExtents().length() * 2.0f, 50.0f);
        _gpuBakeStart = std::chrono::steady_clock::now();
        _gpuLightmapper->bake(_houseMeshes, gpuOptions);
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _house;

    std::unique_ptr<Lightmapper> _lightmapper;
    std::unique_ptr<GpuLightmapper> _gpuLightmapper;
    Lightmapper::Options _bakeOptions;
    Lightmapper::Light _sun;
    Lightmapper::Light _omni;
    Lightmapper::Light _spot;

    BoundingBox _houseBounds;
    std::vector<MeshInstance*> _houseMeshes;

    LightComponent* _directionalLight = nullptr;
    LightComponent* _omniLight = nullptr;
    LightComponent* _spotLight = nullptr;

    // Track unique materials so the ON/OFF toggle can restore them.
    std::vector<std::pair<StandardMaterial*, std::shared_ptr<Texture>>> _bakedMaterials;
    std::unordered_set<StandardMaterial*> _seenMaterials;
    std::vector<std::shared_ptr<Texture>> _keepAlive;  // hold every baked texture

    std::chrono::steady_clock::time_point _gpuBakeStart;

    // Upstream's HUD toggles; changing one re-bakes, exactly as upstream does
    // ("Bake when settings are changed only").
    bool _directionalEnabled = true;   // data.directional.enabled
    bool _otherLightsEnabled = true;   // data.other.enabled
    // DEVIATION: with ambient baking on, the GPU path's virtual lights carry the scene's
    // flat ambient colour rather than the env atlas radiance per direction (upstream's HUD
    // "cubemap" mode), so it tints the scene toward that colour. Off by default therefore —
    // key 3 turns it on to show the occlusion it adds. The CPU baker samples ambient the
    // same way but modulates it by ray-traced AO.
    bool _ambientBakeEnabled = false;  // data.ambient.ambientBake
    bool _hemisphereEnabled = true;    // data.ambient.hemisphere -> spherePart 0.4 vs 1
    bool _filterEnabled = true;        // data.settings.lightmapFilterEnabled
    bool _lightmapOn = true;
};

VISUTWIN_EXAMPLE_MAIN(LightmapBakeExample)
