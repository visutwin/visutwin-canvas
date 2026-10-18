// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// Port of upstream lights-and-shadows/shadow-bounds.
//
// A directional light renders its shadow into a single shadow map fitted to the shadow bounds
// (the black box), which follow the bouncing sphere, keeping its shadow sharp. Compare with the
// shadow map of the same resolution fitted to the camera frustum, spread over the whole shadow
// distance (40). The sphere is a carbon 60 cage of hexagons and pentagons, scaled to a radius of
// 1, bouncing 2.25 high on a 100 x 100 receive-only ground (Ground092C, tiled 25 times); the sun
// (warm white, intensity 2, PCF3 at 512 texels) shines down at an elevation of 45 degrees from an
// azimuth of 30, under the morning env atlas (mip 2, intensity 0.5) at exposure 0.8 with ACES.
//
// Model: "CARBON 60 SPHERE" by Random13, CC BY 4.0
// (https://sketchfab.com/3d-models/carbon-60-sphere-5ef45e632eb8431998400ea73ddc51a5).
//
// Keys stand in for upstream's controls panel:
//   M      the shadow fitted to the bounds, or to the camera frustum
//   T      the shadow type, cycled PCF1, PCF3, PCF5, VSM, PCSS
//   R      the shadow resolution, cycled 256 .. 4096 (512 to begin with)
//   B      show the bounds on / off
//   [ / ]  the sun's elevation -/+ 5 degrees, 10..90
//   S      the sun turning around (10 degrees a second) on / off
//   Space  the bounce on / off
//
// DEVIATIONS:
// - the shadow types are the 32-bit ones; upstream's list also has PCF1/3/5 at 16 bits and
//   VSM at 32, which this engine does not have.
// - upstream's CameraControls with the same focus point, pitch and zoom ranges is the
//   examples' CameraControls in orbit mode.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <vector>

#include "../exampleApp.h"
#include "core/shape/boundingBox.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/extras/wireRenderer.h"
#include "framework/handlers/containerResource.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kGroundSize = 100.0f;
    // The model is roughly 52 units in radius.
    constexpr float kSphereModelRadius = 52.0f;
    constexpr float kSphereRadius = 1.0f;
    constexpr float kBounceHeight = 2.25f;
    constexpr float kBounceSpeed = 2.5f;

    constexpr std::array<ShadowType, 5> kShadowTypes{
        SHADOW_PCF1_32F, SHADOW_PCF3_32F, SHADOW_PCF5_32F, SHADOW_VSM_16F, SHADOW_PCSS_32F};
    constexpr std::array<const char*, 5> kShadowTypeNames{"PCF1", "PCF3", "PCF5", "VSM", "PCSS"};
    constexpr std::array<int, 5> kResolutions{256, 512, 1024, 2048, 4096};
}

class ShadowBoundsExample final : public ExampleApp
{
public:
    ShadowBoundsExample() : ExampleApp({.title = "Shadow Bounds"}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/morning-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _sphereAsset = std::make_unique<Asset>("carbon-sphere", AssetType::CONTAINER,
            assetPath("models/carbon-sphere.glb"));
        Texture* color = load("color", "textures/ground092c/color.png");
        Texture* normal = load("normal", "textures/ground092c/normal.png");
        Texture* roughness = load("roughness", "textures/ground092c/roughness.png");
        Texture* ao = load("ao", "textures/ground092c/ao.png");
        auto* sphereResource = _sphereAsset->resourceAs<ContainerResource>();
        Texture* envAtlas = _envAtlas->resourceAs<Texture>();
        if (!color || !normal || !roughness || !ao || !sphereResource || !envAtlas) {
            spdlog::error("shadow-bounds needs the ground092c textures, carbon-sphere.glb and the morning env atlas");
            return false;
        }

        scene()->setEnvAtlas(envAtlas);
        scene()->setSkyboxMip(2);
        scene()->setSkyboxIntensity(0.5f);
        scene()->setExposure(0.8f);

        // A large ground, which only receives the shadow.
        const Vector2 tiling(kGroundSize / 4.0f, kGroundSize / 4.0f);
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuseMap(color);
        _groundMaterial->setNormalMap(normal);
        _groundMaterial->setAoMap(ao);
        _groundMaterial->setGlossMap(roughness);
        _groundMaterial->setGlossInvert(true);
        _groundMaterial->setUseMetalness(true);
        _groundMaterial->setMetalness(0.0f);
        _groundMaterial->setDiffuseMapTiling(tiling);
        _groundMaterial->setNormalMapTiling(tiling);
        _groundMaterial->setAoMapTiling(tiling);
        _groundMaterial->setGlossMapTiling(tiling);
        Entity* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(kGroundSize, 1.0f, kGroundSize));
        ground->setName("Ground");
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        // The carbon 60 sphere, a cage of hexagons and pentagons, so its shadow shows how sharp
        // the shadow map is.
        _sphere = sphereResource->instantiateRenderEntity();
        _sphere->setEngine(engine());
        for (auto* render : _sphere->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
        }
        const float sphereScale = kSphereRadius / kSphereModelRadius;
        _sphere->setLocalScale(sphereScale, sphereScale, sphereScale);
        root()->addChild(_sphere);

        // The sun. Without the shadow bounds, its shadow map of the same resolution is fitted to
        // the camera frustum, up to the shadow distance.
        _sun = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f), Color(1.0f, 0.95f, 0.85f, 1.0f), 2.0f, true);
        _sun->setName("Sun");
        _sunLight = _sun->findComponent<LightComponent>();
        _sunLight->setNumCascades(1);
        _sunLight->setShadowDistance(40.0f);
        _sunLight->setShadowBias(0.2f);
        _sunLight->setShadowNormalBias(0.02f);
        // The penumbra of PCSS grows by this fraction of the distance from the caster, so the
        // shadow blurs as the sphere bounces up. The falloff of 1 keeps it growing all the way
        // up, larger values soften the shadow sooner.
        _sunLight->setPenumbraSize(0.08f);
        _sunLight->setPenumbraFalloff(1.0f);

        // The camera orbiting the sphere.
        Entity* camera = createCamera(Vector3(-4.5f, 6.0f, 6.5f));
        if (auto* component = camera->findComponent<CameraComponent>()) {
            component->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
            component->camera()->setNearClip(0.1f);
            component->camera()->setFarClip(300.0f);
            component->setToneMapping(TONEMAP_ACES);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 2.5f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-85.0f, -5.0f));
            controls->setZoomRange(Vector2(2.0f, 60.0f));
            controls->storeResetState();
        }

        _wire.emplace(scene().get());
        _wire->setColor(Color(0.0f, 0.0f, 0.0f, 1.0f));

        applySettings();
        spdlog::info("Keys: M bounds / frustum, T shadow type, R resolution, B show bounds, [ ] sun elevation, "
                     "S sun rotate, Space bounce");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_sunLight) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_M: _useBounds = !_useBounds; break;
        case SDLK_T: _typeIndex = (_typeIndex + 1) % kShadowTypes.size(); break;
        case SDLK_R: _resolutionIndex = (_resolutionIndex + 1) % kResolutions.size(); break;
        case SDLK_B: _showBounds = !_showBounds; break;
        case SDLK_LEFTBRACKET: _elevation = std::max(10.0f, _elevation - 5.0f); break;
        case SDLK_RIGHTBRACKET: _elevation = std::min(90.0f, _elevation + 5.0f); break;
        case SDLK_S: _rotateSun = !_rotateSun; break;
        case SDLK_SPACE: _bounce = !_bounce; break;
        default: return false;
        }
        applySettings();
        spdlog::info("shadow {} {} at {}, bounds shown {}, elevation {}, sun rotating {}, bounce {}",
            _useBounds ? "bounds" : "frustum", kShadowTypeNames[_typeIndex], kResolutions[_resolutionIndex],
            _showBounds ? "on" : "off", _elevation, _rotateSun ? "on" : "off", _bounce ? "on" : "off");
        return true;
    }

    void update(const float dt) override
    {
        if (!_sphere || !_sun) {
            return;
        }

        // Bounce the sphere.
        if (_bounce) {
            _bounceTime += dt;
        }
        const Vector3 position(0.0f, kSphereRadius + kBounceHeight * std::abs(std::sin(_bounceTime * kBounceSpeed)),
            0.0f);
        _sphere->setPosition(position);

        // The sun, shining down at the elevation, slowly turning around when enabled.
        if (_rotateSun) {
            _sunAzimuth += dt * 10.0f;
        }
        const float elevation = _elevation * (std::numbers::pi_v<float> / 180.0f);
        const float azimuth = _sunAzimuth * (std::numbers::pi_v<float> / 180.0f);
        const Vector3 direction(-std::cos(elevation) * std::sin(azimuth), -std::sin(elevation),
            -std::cos(elevation) * std::cos(azimuth));
        // The rotation taking the light's -Y onto the direction.
        const Vector3 down(0.0f, -1.0f, 0.0f);
        const Vector3 axis = down.cross(direction);
        const float axisLength = axis.length();
        const float angle = std::atan2(axisLength, down.dot(direction)) * (180.0f / std::numbers::pi_v<float>);
        _sun->setRotation(axisLength > 1e-6f ? Quaternion::fromAxisAngle(axis * (1.0f / axisLength), angle)
                                             : Quaternion());

        // The shadow bounds follow the sphere, or the shadow is fitted to the camera frustum.
        // They only need to cover the sphere, as the ground below it, further along the light,
        // is shadowed by the casters within the bounds.
        if (_useBounds) {
            const BoundingBox bounds(position, Vector3(kSphereRadius, kSphereRadius, kSphereRadius));
            _sunLight->setShadowBounds(bounds);
            if (_showBounds) {
                _wire->box(bounds);
            }
        } else {
            _sunLight->setShadowBounds(std::nullopt);
        }
    }

private:
    Texture* load(const char* name, const char* relative)
    {
        _assets.push_back(std::make_unique<Asset>(name, AssetType::TEXTURE, assetPath(relative)));
        return _assets.back()->resourceAs<Texture>();
    }

    void applySettings()
    {
        _sunLight->setShadowType(kShadowTypes[_typeIndex]);
        _sunLight->setShadowResolution(kResolutions[_resolutionIndex]);
        // VSM blurs over a number of texels. The texels of the shadow bounds are much smaller
        // than the texels of the shadow fitted to the camera frustum, so the bounds use the
        // widest blur to look soft, which would blur the other shadow away.
        _sunLight->setVsmBlurSize(_useBounds ? 25 : 11);
        _sunLight->setVsmBlurMode(_useBounds ? BLUR_BOX : BLUR_GAUSSIAN);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _sphereAsset;
    std::vector<std::unique_ptr<Asset>> _assets;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    Entity* _sphere = nullptr;
    Entity* _sun = nullptr;
    LightComponent* _sunLight = nullptr;
    std::optional<WireRenderer> _wire;

    bool _useBounds = true;
    size_t _typeIndex = 1;        // PCF3
    size_t _resolutionIndex = 1;  // 512
    bool _showBounds = true;
    float _elevation = 45.0f;
    bool _rotateSun = false;
    bool _bounce = true;
    float _bounceTime = 0.0f;
    float _sunAzimuth = 30.0f;
};

VISUTWIN_EXAMPLE_MAIN(ShadowBoundsExample)
