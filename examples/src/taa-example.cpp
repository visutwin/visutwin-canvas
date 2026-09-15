// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/taa.
//
// The PBR house (scaled 100x) under the table-mountain env atlas (skybox mip 0,
// exposure 2.5), a shadow-casting directional light, and a cube orbiting the
// house at radius 130 while it spins. The camera frame runs TAA (jitter 1),
// ACES tone mapping, bloom 0.02 and sharpness 0.5; an orbit camera frames the
// house on start.
//
// Keys stand in for upstream's controls panel, over the same parameters:
//   T      TAA enabled (like upstream, turning it on sets sharpness 1, off 0)
//   [ / ]  TAA jitter -/+ 0.05, 0..1
//   S      sharpness, cycled 0 / 0.25 / 0.5 / 0.75 / 1
//   B      bloom on (0.02) / off
//   - / =  render target scale -/+ 0.1, 0.5..1
//
// DEVIATIONS:
// - upstream's orbitCamera script (inertia 0.2, distanceMax 400, frameOnStart)
//   is replaced by the examples' CameraControls in orbit mode. The start pose is
//   computed the way frameOnStart does it: pivot at the house's AABB centre, the
//   viewing direction from the authored camera position (0, 40, -220), distance
//   1.5 * max half-extent / sin(fov / 2), clamped to 400. There is no inertia.
//
#include <algorithm>
#include <cmath>
#include <memory>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class TaaExample final: public ExampleApp
{
public:
    TaaExample(): ExampleApp({.title = "TAA Example"}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>(
            "env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _house = std::make_unique<Asset>(
            "house", AssetType::CONTAINER, assetPath("models/pbr-house.glb"));
        _cube = std::make_unique<Asset>(
            "cube", AssetType::CONTAINER, assetPath("models/playcanvas-cube.glb"));

        // Setup skydome with low intensity
        if (const auto envAtlasResource = _envAtlas->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
        } else {
            spdlog::warn("Failed to load environment atlas — continuing without IBL");
        }
        scene()->setSkyboxMip(0);
        scene()->setExposure(2.5f);

        // Create an instance of the house and add it to the scene
        const auto houseResource = _house->resource();
        if (!houseResource) {
            spdlog::error("Failed to load house model");
            return false;
        }
        auto* houseEntity = std::get<ContainerResource*>(*houseResource)->instantiateRenderEntity();
        houseEntity->setLocalScale(100, 100, 100);
        root()->addChild(houseEntity);

        // Frame the house the way upstream's orbitCamera frameOnStart does.
        constexpr float fov = 80.0f;
        const auto houseBbox = entityBounds(houseEntity);
        const Vector3 pivot = houseBbox.center();
        const Vector3 halfExtents = houseBbox.halfExtents();
        const float radius = std::max({halfExtents.getX(), halfExtents.getY(), halfExtents.getZ()});
        const float distance = std::min(radius * 1.5f / std::sin(0.5f * fov * DEG_TO_RAD), 400.0f);
        const Vector3 authoredPosition(0.0f, 40.0f, -220.0f);
        const Vector3 cameraPosition = pivot + (authoredPosition - pivot).normalized() * distance;

        // Create an Entity with a camera component
        auto* camera = createCamera(cameraPosition);
        _cameraComp = camera->findComponent<CameraComponent>();
        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->camera()->setNearClip(10.0f);
            _cameraComp->camera()->setFarClip(600.0f);
            _cameraComp->camera()->setFov(fov);
        }

        auto* controls = addOrbitControls(camera, pivot);
        if (controls) {
            controls->setZoomRange(Vector2(0.0f, 400.0f));
            controls->storeResetState();
        }

        // Add a shadow casting directional light
        auto* light = createDirectionalLight(Vector3(40.0f, 10.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(4096);
            lightComp->setShadowDistance(600.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        if (const auto cubeResource = _cube->resource()) {
            _cubeEntity = std::get<ContainerResource*>(*cubeResource)->instantiateRenderEntity();
            _cubeEntity->setLocalScale(30, 30, 30);
            root()->addChild(_cubeEntity);
        }

        // Camera frame: ACES, bloom 0.02, then the initial control values
        // (scale 1, bloom on, sharpness 0.5, TAA on with jitter 1).
        if (_cameraComp) {
            _cameraComp->setToneMapping(TONEMAP_ACES);

            auto taa = _cameraComp->taa();
            taa.enabled = true;
            taa.jitter = 1.0f;
            _cameraComp->setTaa(taa);

            auto rendering = _cameraComp->rendering();
            rendering.bloomIntensity = 0.02f;
            rendering.sharpness = 0.5f;
            rendering.renderTargetScale = 1.0f;
            _cameraComp->setRendering(rendering);
        }

        spdlog::info("Controls: T TAA, [ ] jitter, S sharpness, B bloom, -/= resolution scale");
        logState("init");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }

        auto taa = _cameraComp->taa();
        auto rendering = _cameraComp->rendering();

        switch (event.key.key) {
        case SDLK_T:
            taa.enabled = !taa.enabled;
            // TAA has been flipped, setup sharpening appropriately (as upstream).
            rendering.sharpness = taa.enabled ? 1.0f : 0.0f;
            break;
        case SDLK_LEFTBRACKET:
            taa.jitter = std::max(0.0f, taa.jitter - 0.05f);
            break;
        case SDLK_RIGHTBRACKET:
            taa.jitter = std::min(1.0f, taa.jitter + 0.05f);
            break;
        case SDLK_S:
            if (rendering.sharpness < 0.125f) rendering.sharpness = 0.25f;
            else if (rendering.sharpness < 0.375f) rendering.sharpness = 0.5f;
            else if (rendering.sharpness < 0.625f) rendering.sharpness = 0.75f;
            else if (rendering.sharpness < 0.875f) rendering.sharpness = 1.0f;
            else rendering.sharpness = 0.0f;
            break;
        case SDLK_B:
            rendering.bloomIntensity = rendering.bloomIntensity > 0.0f ? 0.0f : 0.02f;
            break;
        case SDLK_MINUS:
            rendering.renderTargetScale = std::max(0.5f, rendering.renderTargetScale - 0.1f);
            break;
        case SDLK_EQUALS:
            rendering.renderTargetScale = std::min(1.0f, rendering.renderTargetScale + 0.1f);
            break;
        default:
            return false;
        }

        _cameraComp->setTaa(taa);
        _cameraComp->setRendering(rendering);
        logState("changed");
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
    void logState(const char* reason) const
    {
        const auto& taa = _cameraComp->taa();
        const auto& rendering = _cameraComp->rendering();
        spdlog::info("{}: taa={}, jitter={:.2f}, sharpness={:.2f}, bloom={:.2f}, scale={:.1f}",
            reason, taa.enabled ? "ON" : "OFF", taa.jitter, rendering.sharpness,
            rendering.bloomIntensity, rendering.renderTargetScale);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _house;
    std::unique_ptr<Asset> _cube;

    CameraComponent* _cameraComp = nullptr;
    Entity* _cubeEntity = nullptr;

    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TaaExample)
