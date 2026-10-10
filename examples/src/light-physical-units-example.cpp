// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream graphics/light-physical-units.
//
// Lights measured in physical units, seen through a physical camera. Two SheenChair.glb
// chairs (scale 3) stand at (7, -1, 0) and (4, -1, 0), the second switched to its
// "Peacock Velvet" material variant, beside Lights.glb at (10, 0, 0): a cube with the
// file's four KHR_lights_punctual lights (red point, green and blue spots, cyan sun), all
// enabled. They stand on a 100 x 100 seaside-rocks ground at y = -1 (colour, normal and
// gloss maps tiled 17x, gloss 0.8, metalness 0.7) under the helipad environment atlas
// (skybox mip 1). Four lights are added, each given a LUMINANCE rather than an intensity:
//   - a white shadowed directional sun at (45, 35, 0), 100000 lux,
//   - a white omni at (0, 5, 0), 100000 cd (range 10, linear falloff),
//   - a white spot at (10, 5, 5) pointing down, 200000, outer cone 45, inner 0,
//   - a yellow 4 x 5 rect area light at (5, 3, -5) turned (70, 180, 0), 200000, range
//     9999, inverse-squared falloff, cones 80 / 85, with an emissive yellow plane showing
//     its source at an emissive intensity equal to its luminance.
// The ambient is red (1, 0, 0) at a luminance of 20000 and the sky at 20000; physical units
// are on. The camera at (0, 5, 11) clears to (0.4, 0.45, 0.5), exposes at f/16, 1/1000 s and
// ISO 1000, requests the scene colour map, and orbits the first chair's bounds centre
// (distance 1 to 400). Area lights are enabled for the clustered lighting.
//
// Keys stand in for upstream's control panel:
//   1 / 2   rect luminance -/+ 50000, 0..800000
//   3 / 4   point luminance -/+ 50000, 0..800000
//   5 / 6   spot luminance -/+ 25000, 0..200000
//   7 / 8   spot angle -/+ 5 degrees, 1..90
//   9 / 0   camera aperture (f/x) -/+ 1, 1..16
//   - / =   camera shutter (1/x s): x halved / doubled, 1..1000
//   [ / ]   camera sensitivity (ISO) -/+ 100, 100..1000
//   ; / '   sky luminance -/+ 5000, 0..100000
//   , / .   sun luminance -/+ 5000, 0..100000
//   Space   animate the aperture (3 + (1 + sin t) x 5) on/off
//   P       physical units on/off
//   K       skylight (the environment atlas) on/off
//
// DEVIATION: upstream loads the area-light LTC LUTs from a JSON asset; this engine has them
// built in.
// DEVIATION: the scene has no skybox or ambient LUMINANCE. Under physical units upstream
// shades the environment (sky, reflections and ambient) with the sky luminance in place of
// the skybox intensity, and multiplies the decoded ambient colour by the ambient luminance;
// the example applies the same numbers by hand whenever the units change: the skybox
// intensity becomes the sky luminance (1 without physical units) and the ambient colour
// is authored so that it decodes to (20000, 0, 0) (to (1, 0, 0) without).
// DEVIATION: the chairs' KHR_materials_sheen textures are ignored (the engine applies the
// extension's factors only), so the velvet's sheen is uniform across the fabric.
// DEVIATION: upstream's orbitCamera script (inertia 0.2) is the examples' CameraControls in
// orbit mode around the same pivot, with its own damping.
// DEVIATION: upstream caps the pixel ratio at 2; the examples render at one pixel per point.
//
// Upstream's settings name ACES tone mapping but never apply it, so the camera keeps the
// default linear tone mapping, as upstream renders.
//
// @credit
// title: Sheen Chair
// author: Wayfair LLC
// source: https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/SheenChair
// license: CC BY 4.0 (http://creativecommons.org/licenses/by/4.0/)
//
#include <algorithm>
#include <cmath>
#include <memory>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kGroundTiling = 17.0f;

    // The ambient colour is red at this luminance under physical units.
    constexpr float kAmbientLuminance = 20000.0f;

    // Key steps; the ranges are the control panel's slider ranges.
    constexpr float kRectStep = 50000.0f;
    constexpr float kRectMax = 800000.0f;
    constexpr float kPointStep = 50000.0f;
    constexpr float kPointMax = 800000.0f;
    constexpr float kSpotStep = 25000.0f;
    constexpr float kSpotMax = 200000.0f;
    constexpr float kSpotAngleStep = 5.0f;
    constexpr float kApertureStep = 1.0f;
    constexpr float kSensitivityStep = 100.0f;
    constexpr float kSkyStep = 5000.0f;
    constexpr float kSkyMax = 100000.0f;
    constexpr float kSunStep = 5000.0f;
    constexpr float kSunMax = 100000.0f;
}

class LightPhysicalUnitsExample final: public ExampleApp
{
public:
    LightPhysicalUnitsExample(): ExampleApp({.title = "Light Physical Units"}) {}

protected:
    bool create() override
    {
        // ------ Assets ------
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _lightsAsset = std::make_unique<Asset>("lights", AssetType::CONTAINER, assetPath("models/Lights.glb"));
        _sheenAsset = std::make_unique<Asset>("sheen", AssetType::CONTAINER, assetPath("models/SheenChair.glb"));
        _colorAsset = std::make_unique<Asset>("color", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-color.jpg"), AssetData{.mipmaps = true});
        _normalAsset = std::make_unique<Asset>("normal", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-normal.jpg"), AssetData{.mipmaps = true});
        _glossAsset = std::make_unique<Asset>("gloss", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-gloss.jpg"), AssetData{.mipmaps = true});

        _helipad = requireTexture(_helipadAsset, "helipad-env-atlas");
        Texture* colorTexture = requireTexture(_colorAsset, "seaside-rocks01-color");
        Texture* normalTexture = requireTexture(_normalAsset, "seaside-rocks01-normal");
        Texture* glossTexture = requireTexture(_glossAsset, "seaside-rocks01-gloss");
        auto* lightsContainer = loadContainer(_lightsAsset);
        auto* sheenContainer = loadContainer(_sheenAsset);
        if (!_helipad || !colorTexture || !normalTexture || !glossTexture || !lightsContainer
            || !sheenContainer) {
            return false;
        }

        // ------ Scene ------
        scene()->setSkyboxMip(1);

        // Area lights are disabled by default for clustered lighting.
        scene()->lighting().areaLightsEnabled = true;

        // ------ Chairs ------
        Entity* sheen1 = sheenContainer->instantiateRenderEntity();
        Entity* sheen2 = sheenContainer->instantiateRenderEntity();
        if (!sheen1 || !sheen2) {
            spdlog::error("Failed to instantiate SheenChair.glb");
            return false;
        }
        sheen1->setEngine(engine());
        sheen1->setLocalScale(3.0f, 3.0f, 3.0f);
        sheen1->setLocalPosition(7.0f, -1.0f, 0.0f);
        root()->addChild(sheen1);

        sheen2->setEngine(engine());
        sheen2->setLocalScale(3.0f, 3.0f, 3.0f);
        sheen2->setLocalPosition(4.0f, -1.0f, 0.0f);
        sheenContainer->applyMaterialVariant(sheen2, "Peacock Velvet");
        root()->addChild(sheen2);

        // ------ The glb's lights, all enabled (imported disabled) ------
        Entity* lights = lightsContainer->instantiateRenderEntity();
        if (!lights) {
            spdlog::error("Failed to instantiate Lights.glb");
            return false;
        }
        lights->setEngine(engine());
        for (auto* component : lights->findComponents<LightComponent>()) {
            component->setEnabled(true);
        }
        lights->setLocalPosition(10.0f, 0.0f, 0.0f);
        root()->addChild(lights);

        // ------ Ground ------
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuseMap(colorTexture);
        _groundMaterial->setNormalMap(normalTexture);
        _groundMaterial->setGloss(0.8f);
        _groundMaterial->setGlossMap(glossTexture);
        _groundMaterial->setMetalness(0.7f);
        _groundMaterial->setUseMetalness(true);
        const Vector2 groundTiling(kGroundTiling, kGroundTiling);
        _groundMaterial->setDiffuseMapTiling(groundTiling);
        _groundMaterial->setNormalMapTiling(groundTiling);
        _groundMaterial->setGlossMapTiling(groundTiling);

        auto* plane = new Entity();
        plane->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(plane->addComponent<RenderComponent>())) {
            render->setMaterial(_groundMaterial.get());
            render->setType("plane");
        }
        plane->setLocalScale(100.0f, 0.0f, 100.0f);
        plane->setLocalPosition(0.0f, -1.0f, 0.0f);
        root()->addChild(plane);

        // ------ Directional light ------
        auto* directionalLight = new Entity();
        directionalLight->setEngine(engine());
        _sun = static_cast<LightComponent*>(directionalLight->addComponent<LightComponent>());
        _sun->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        _sun->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _sun->setCastShadows(true);
        _sun->setLuminance(_sunLuminance);
        _sun->setShadowBias(0.2f);
        _sun->setShadowNormalBias(0.05f);
        _sun->setShadowResolution(2048);
        directionalLight->setLocalEulerAngles(45.0f, 35.0f, 0.0f);
        root()->addChild(directionalLight);

        // ------ Omni light ------
        auto* omniLight = new Entity();
        omniLight->setEngine(engine());
        _omni = static_cast<LightComponent*>(omniLight->addComponent<LightComponent>());
        _omni->setType(LightType::LIGHTTYPE_OMNI);
        _omni->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _omni->setCastShadows(false);
        _omni->setLuminance(_pointLuminance);
        _omni->setShadowBias(0.2f);
        _omni->setShadowNormalBias(0.05f);
        _omni->setShadowResolution(2048);
        omniLight->setLocalPosition(0.0f, 5.0f, 0.0f);
        root()->addChild(omniLight);

        // ------ Spot light ------
        auto* spotLight = new Entity();
        spotLight->setEngine(engine());
        _spot = static_cast<LightComponent*>(spotLight->addComponent<LightComponent>());
        _spot->setType(LightType::LIGHTTYPE_SPOT);
        _spot->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _spot->setCastShadows(false);
        _spot->setLuminance(_spotLuminance);
        _spot->setShadowBias(0.2f);
        _spot->setShadowNormalBias(0.05f);
        _spot->setShadowResolution(2048);
        _spot->setOuterConeAngle(_spotAngle);
        _spot->setInnerConeAngle(0.0f);
        spotLight->setLocalEulerAngles(0.0f, 0.0f, 0.0f);
        spotLight->setLocalPosition(10.0f, 5.0f, 5.0f);
        root()->addChild(spotLight);

        // ------ Rect area light ------
        const Color yellow(1.0f, 1.0f, 0.0f, 1.0f);
        auto* areaLight = new Entity();
        areaLight->setEngine(engine());
        _rect = static_cast<LightComponent*>(areaLight->addComponent<LightComponent>());
        _rect->setType(LightType::LIGHTTYPE_SPOT);
        _rect->setShape(LightShape::LIGHTSHAPE_RECT);
        _rect->setColor(yellow);
        _rect->setRange(9999.0f);
        _rect->setLuminance(_rectLuminance);
        _rect->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
        _rect->setInnerConeAngle(80.0f);
        _rect->setOuterConeAngle(85.0f);
        _rect->setShadowNormalBias(0.1f);
        areaLight->setLocalScale(4.0f, 1.0f, 5.0f);
        areaLight->setLocalEulerAngles(70.0f, 180.0f, 0.0f);
        areaLight->setLocalPosition(5.0f, 3.0f, -5.0f);

        // Emissive material that is the light source colour.
        _brightMaterial = std::make_shared<StandardMaterial>();
        _brightMaterial->setEmissive(yellow);
        _brightMaterial->setEmissiveIntensity(_rect->luminance());
        _brightMaterial->setUseLighting(false);
        _brightMaterial->setCullMode(CullMode::CULLFACE_NONE);

        // Primitive shape that matches the light source shape.
        auto* brightShape = new Entity();
        brightShape->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(brightShape->addComponent<RenderComponent>())) {
            render->setMaterial(_brightMaterial.get());
            render->setType("plane");
            render->setCastShadows(false);
        }
        areaLight->addChild(brightShape);
        root()->addChild(areaLight);

        // ------ Camera ------
        auto* camera = createCamera(Vector3(0.0f, 5.0f, 11.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            _camera = cameraComponent->camera();
            _camera->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
            cameraComponent->requestSceneColorMap(true);
        }
        if (!_camera) {
            spdlog::error("Camera entity has no camera");
            return false;
        }
        applyCamera();

        // The orbit pivots on the first chair's bounds centre, keeping the distance the
        // camera starts at.
        const Vector3 pivot = entityBounds(sheen1).center();
        camera->lookAt(pivot);
        if (auto* controls = addOrbitControls(camera, pivot)) {
            controls->setZoomRange(Vector2(1.0f, 400.0f));
            controls->storeResetState();
        }

        applyPhysicalUnits();
        applySky();

        spdlog::info("Keys: 1/2 rect, 3/4 point, 5/6 spot luminance, 7/8 spot angle, 9/0 aperture, "
                     "-/= shutter, [/] ISO, ;/' sky, ,/. sun luminance, Space animate, "
                     "P physical units, K skylight, R reset camera, Esc quit");
        logState();
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        handleKeys();

        if (_animate) {
            _aperture = 3.0f + (1.0f + std::sin(_time)) * 5.0f;
            applyCamera();
        }
    }

private:
    static Texture* requireTexture(const std::unique_ptr<Asset>& asset, const char* label)
    {
        Texture* texture = asset->resourceAs<Texture>();
        if (!texture) {
            spdlog::error("Failed to load texture asset '{}'", label);
        }
        return texture;
    }

    static GlbContainerResource* loadContainer(const std::unique_ptr<Asset>& asset)
    {
        ContainerResource* container = asset->resourceAs<ContainerResource>();
        auto* glb = dynamic_cast<GlbContainerResource*>(container);
        if (!glb) {
            spdlog::error("GLB '{}' failed to load as a container", asset->name());
        }
        return glb;
    }

    // A slider value stepped down and up, clamped to the slider's range. True when it moved.
    static bool step(float& value, const bool down, const bool up, const float amount,
        const float minimum, const float maximum)
    {
        const float before = value;
        if (down) {
            value -= amount;
        }
        if (up) {
            value += amount;
        }
        value = std::clamp(value, minimum, maximum);
        return value != before;
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }

        bool changed = false;

        if (step(_rectLuminance, keyboard->wasPressed(Key::Digit1), keyboard->wasPressed(Key::Digit2),
                kRectStep, 0.0f, kRectMax)) {
            _rect->setLuminance(_rectLuminance);
            _brightMaterial->setEmissiveIntensity(_rectLuminance);
            changed = true;
        }
        if (step(_pointLuminance, keyboard->wasPressed(Key::Digit3), keyboard->wasPressed(Key::Digit4),
                kPointStep, 0.0f, kPointMax)) {
            _omni->setLuminance(_pointLuminance);
            changed = true;
        }
        if (step(_spotLuminance, keyboard->wasPressed(Key::Digit5), keyboard->wasPressed(Key::Digit6),
                kSpotStep, 0.0f, kSpotMax)) {
            _spot->setLuminance(_spotLuminance);
            changed = true;
        }
        if (step(_spotAngle, keyboard->wasPressed(Key::Digit7), keyboard->wasPressed(Key::Digit8),
                kSpotAngleStep, 1.0f, 90.0f)) {
            _spot->setOuterConeAngle(_spotAngle);
            changed = true;
        }

        bool cameraChanged = step(_aperture, keyboard->wasPressed(Key::Digit9),
            keyboard->wasPressed(Key::Digit0), kApertureStep, 1.0f, 16.0f);
        if (keyboard->wasPressed(Key::Minus) || keyboard->wasPressed(Key::Equals)) {
            const float before = _shutter;
            if (keyboard->wasPressed(Key::Minus)) {
                _shutter *= 0.5f;
            }
            if (keyboard->wasPressed(Key::Equals)) {
                _shutter *= 2.0f;
            }
            _shutter = std::clamp(_shutter, 1.0f, 1000.0f);
            cameraChanged = cameraChanged || _shutter != before;
        }
        cameraChanged = step(_sensitivity, keyboard->wasPressed(Key::LeftBracket),
            keyboard->wasPressed(Key::RightBracket), kSensitivityStep, 100.0f, 1000.0f) || cameraChanged;
        if (cameraChanged) {
            applyCamera();
            changed = true;
        }

        if (step(_skyLuminance, keyboard->wasPressed(Key::Semicolon), keyboard->wasPressed(Key::Apostrophe),
                kSkyStep, 0.0f, kSkyMax)) {
            applyPhysicalUnits();
            changed = true;
        }
        if (step(_sunLuminance, keyboard->wasPressed(Key::Comma), keyboard->wasPressed(Key::Period),
                kSunStep, 0.0f, kSunMax)) {
            _sun->setLuminance(_sunLuminance);
            changed = true;
        }

        if (keyboard->wasPressed(Key::Space)) {
            _animate = !_animate;
            changed = true;
        }
        if (keyboard->wasPressed(Key::P)) {
            _physicalUnits = !_physicalUnits;
            applyPhysicalUnits();
            changed = true;
        }
        if (keyboard->wasPressed(Key::K)) {
            _sky = !_sky;
            applySky();
            changed = true;
        }

        if (changed) {
            logState();
        }
    }

    // The physical camera: aperture in f-stops, the shutter given as 1/x seconds, ISO.
    void applyCamera() const
    {
        _camera->setAperture(_aperture);
        _camera->setShutter(1.0f / _shutter);
        _camera->setSensitivity(_sensitivity);
    }

    // Physical units switch the lights to their luminance and the camera to its physical
    // exposure (both in the engine), and the sky and ambient to their luminances (here, see
    // the header).
    void applyPhysicalUnits() const
    {
        scene()->setPhysicalUnits(_physicalUnits);
        scene()->setSkyboxIntensity(_physicalUnits ? _skyLuminance : 1.0f);
        // Authored gamma space: decodes to (luminance, 0, 0), or to (1, 0, 0) without units.
        const float ambientRed = _physicalUnits ? std::pow(kAmbientLuminance, 1.0f / 2.2f) : 1.0f;
        scene()->setAmbientLight(ambientRed, 0.0f, 0.0f);
    }

    // Without the skylight the scene has no environment: the camera's clear colour shows
    // and the red ambient lights the scene.
    void applySky() const
    {
        scene()->setEnvAtlas(_sky ? _helipad : nullptr);
    }

    void logState() const
    {
        spdlog::info("Rect {:.0f}, point {:.0f}, spot {:.0f} (angle {:.0f}), sun {:.0f}, sky {:.0f} | "
                     "f/{:.1f}, 1/{:.1f} s, ISO {:.0f}, exposure {:.3g} | animate {}, physical {}, skylight {}",
            _rectLuminance, _pointLuminance, _spotLuminance, _spotAngle, _sunLuminance, _skyLuminance,
            _aperture, _shutter, _sensitivity, scene()->exposureFor(_camera),
            _animate ? "on" : "off", _physicalUnits ? "on" : "off", _sky ? "on" : "off");
    }

    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<Asset> _lightsAsset;
    std::unique_ptr<Asset> _sheenAsset;
    std::unique_ptr<Asset> _colorAsset;
    std::unique_ptr<Asset> _normalAsset;
    std::unique_ptr<Asset> _glossAsset;

    Texture* _helipad = nullptr;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<StandardMaterial> _brightMaterial;

    LightComponent* _sun = nullptr;
    LightComponent* _omni = nullptr;
    LightComponent* _spot = nullptr;
    LightComponent* _rect = nullptr;
    Camera* _camera = nullptr;

    // The control panel's initial settings.
    float _sunLuminance = 100000.0f;
    float _skyLuminance = 20000.0f;
    float _spotLuminance = 200000.0f;
    float _spotAngle = 45.0f;
    float _pointLuminance = 100000.0f;
    float _rectLuminance = 200000.0f;
    float _aperture = 16.0f;
    float _shutter = 1000.0f;
    float _sensitivity = 1000.0f;
    bool _animate = false;
    bool _physicalUnits = true;
    bool _sky = true;

    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(LightPhysicalUnitsExample)
