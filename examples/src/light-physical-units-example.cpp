// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream lights-and-shadows/light-physical-units.
//
// Lights specified in physical units (lumens, lux), exposed through a camera with aperture,
// shutter and ISO, in a dark gallery. A row of four SheenChair.glb chairs (scale 3) stands at
// y = -1 in alternating fabrics, the outer two turned in a little:
//   (0.3, -2.5) yaw 15 "Peacock Velvet", (4, 0) "Peacock Velvet", (7, 0) "Mango Velvet",
//   (10, -2.5) yaw -15 "Mango Velvet".
// They stand on a dark polished 300 x 300 floor at y = -1 (diffuse 0.15, 0.15, 0.17, specular
// workflow with specular 0.7, gloss 0.65). The helipad environment atlas (skybox mip 1) only
// lights the scene: the Skybox layer is disabled, the camera clears to the background colour
// (0.02, 0.022, 0.03), and an EXP2 fog of that colour (density 0.03) fades the floor into it.
// Four lights are added, each given a LUMINANCE rather than an intensity:
//   - a warm (1, 0.94, 0.85) shadowed directional sun at (45, 35, 0), 100000 lux,
//   - a pink (1, 0.4, 0.75) omni at (-0.5, 1.2, -5), 1200000, range 7, inverse-squared falloff,
//   - a blue (0.55, 0.75, 1) spot at (10, 1.5, -5) aimed at (9.5, 0, 0), 600000, range 8,
//     inverse-squared falloff, outer cone 30, inner 0,
//   - an amber (1, 0.72, 0.38) 4 x 5 rect area light at (5, 3, -5) turned (70, 180, 0), 200000,
//     range 9999, inverse-squared falloff, cones 80 / 85, with an emissive plane of its colour
//     showing its source at an emissive intensity equal to its luminance.
// The ambient is (0.5, 0.55, 0.7) at a luminance of 4000 and the sky at 6000; physical units
// are on. Area lights are enabled for the clustered lighting, the rectangle alone (the disk and
// sphere shapes are switched off, so no shader compiles their code). The camera at (11, 4, 13)
// has a 40 degree field of view, exposes at f/16, 1/1000 s and ISO 1000, requests the scene
// colour map, and orbits (6.5, 1.5, -1) (distance 1 to 400, pitch within +/-90 degrees, no fly).
// It renders through a camera frame with ACES tone mapping and a vignette of intensity 0.6. A
// light gizmo, without handles and in the light's colour, draws the omni and spot lights' shapes
// in a "Helpers" layer that keeps the scene depth.
//
// Keys stand in for upstream's control panel:
//   Z / X / C  rect / point / spot light on/off (the rect hides its emissive shape too)
//   1 / 2      rect luminance -/+ 50000, 0..800000
//   3 / 4      point luminance -/+ 100000, 0..2000000
//   5 / 6      spot luminance -/+ 50000, 0..1000000
//   7 / 8      spot angle -/+ 5 degrees, 1..90
//   9 / 0      camera aperture (f/x) -/+ 1, 1..16
//   - / =      camera shutter (1/x s): x halved / doubled, 1..8000
//   [ / ]      camera sensitivity (ISO) -/+ 100, 100..1000
//   ; / '      sky luminance -/+ 2000, 0..100000
//   , / .      sun luminance -/+ 5000, 0..100000
//   Space      animate the aperture (3 + (1 + sin t) x 5) on/off
//   P          physical units on/off
//   K          skylight (the environment atlas) on/off
//
// DEVIATION: upstream loads the area-light LTC LUTs from a JSON asset; this engine has them
// built in.
// DEVIATION: the scene has no skybox or ambient LUMINANCE. Under physical units upstream
// shades the environment (reflections and ambient) with the sky luminance in place of the
// skybox intensity, and multiplies the decoded ambient colour by the ambient luminance; the
// example applies the same numbers by hand whenever the units change: the skybox intensity
// becomes the sky luminance (1 without physical units) and the ambient colour is authored so
// that it decodes to the ambient colour times 4000 (to the colour alone without).
// DEVIATION: the chairs' KHR_materials_sheen textures are ignored (the engine applies the
// extension's factors only), so the velvet's sheen is uniform across the fabric.
// DEVIATION: upstream caps the pixel ratio at 2; the examples render at one pixel per point.
// DEVIATION: upstream's CameraFrame object is the camera component's rendering settings here
// (the vignette switched on with its intensity), and its tone mapping the camera's own.
//
// @credit
// title: Sheen Chair
// author: Wayfair LLC
// source: https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/SheenChair
// license: CC BY 4.0 (http://creativecommons.org/licenses/by/4.0/)
//
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <memory>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "core/math/color.h"
#include "framework/assets/asset.h"
#include "framework/gizmo/lightGizmo.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The dark gallery's background: the clear colour and the fog's.
    const Color kBackgroundColor(0.02f, 0.022f, 0.03f, 1.0f);

    // The ambient colour, at this luminance under physical units.
    constexpr std::array<float, 3> kAmbientColor = {0.5f, 0.55f, 0.7f};
    constexpr float kAmbientLuminance = 4000.0f;

    // Key steps; the ranges are the control panel's slider ranges.
    constexpr float kRectStep = 50000.0f;
    constexpr float kRectMax = 800000.0f;
    constexpr float kPointStep = 100000.0f;
    constexpr float kPointMax = 2000000.0f;
    constexpr float kSpotStep = 50000.0f;
    constexpr float kSpotMax = 1000000.0f;
    constexpr float kSpotAngleStep = 5.0f;
    constexpr float kApertureStep = 1.0f;
    constexpr float kShutterMax = 8000.0f;
    constexpr float kSensitivityStep = 100.0f;
    constexpr float kSkyStep = 2000.0f;
    constexpr float kSkyMax = 100000.0f;
    constexpr float kSunStep = 5000.0f;
    constexpr float kSunMax = 100000.0f;

    struct ChairPlacement
    {
        float x;
        float z;
        float yaw;
        const char* variant;
    };

    // A row of chairs in alternating fabrics, the outer ones turned in a little.
    constexpr std::array<ChairPlacement, 4> kChairs = {{
        {0.3f, -2.5f, 15.0f, "Peacock Velvet"},
        {4.0f, 0.0f, 0.0f, "Peacock Velvet"},
        {7.0f, 0.0f, 0.0f, "Mango Velvet"},
        {10.0f, -2.5f, -15.0f, "Mango Velvet"},
    }};
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
        _sheenAsset = std::make_unique<Asset>("sheen", AssetType::CONTAINER, assetPath("models/SheenChair.glb"));

        _helipad = _helipadAsset->resourceAs<Texture>();
        if (!_helipad) {
            spdlog::error("Failed to load texture asset 'helipad-env-atlas'");
        }
        auto* sheenContainer = loadContainer(_sheenAsset);
        if (!_helipad || !sheenContainer) {
            return false;
        }

        // ------ Scene ------
        // Dark gallery look: the environment only provides soft ambient light, the skybox
        // itself is hidden.
        scene()->setSkyboxMip(1);
        if (const auto& layers = scene()->layers()) {
            if (const auto skyboxLayer = layers->getLayerById(LAYERID_SKYBOX)) {
                skyboxLayer->setEnabled(false);
            }
        }

        // Fade the floor into the background.
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_EXP2);
        scene()->setFogColor(kBackgroundColor);
        scene()->setFogDensity(0.03f);

        // Area lights are disabled by default for clustered lighting.
        scene()->lighting().areaLightsEnabled = true;

        // The only area light is a rectangle, so the materials' shaders compile no disk and
        // sphere light code.
        scene()->lighting().areaLightDiskEnabled = false;
        scene()->lighting().areaLightSphereEnabled = false;

        // ------ Chairs ------
        for (const auto& placement : kChairs) {
            Entity* chair = sheenContainer->instantiateRenderEntity();
            if (!chair) {
                spdlog::error("Failed to instantiate SheenChair.glb");
                return false;
            }
            chair->setEngine(engine());
            chair->setLocalScale(3.0f, 3.0f, 3.0f);
            chair->setLocalPosition(placement.x, -1.0f, placement.z);
            chair->setLocalEulerAngles(0.0f, placement.yaw, 0.0f);
            sheenContainer->applyMaterialVariant(chair, placement.variant);
            root()->addChild(chair);
        }

        // ------ Dark polished floor ------
        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.15f, 0.15f, 0.17f, 1.0f));
        _floorMaterial->setGloss(0.65f);
        _floorMaterial->setUseMetalness(false);
        _floorMaterial->setSpecular(Color(0.7f, 0.7f, 0.7f, 1.0f));

        auto* plane = new Entity();
        plane->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(plane->addComponent<RenderComponent>())) {
            render->setMaterial(_floorMaterial.get());
            render->setType("plane");
        }
        plane->setLocalScale(300.0f, 0.0f, 300.0f);
        plane->setLocalPosition(0.0f, -1.0f, 0.0f);
        root()->addChild(plane);

        // ------ Directional light ------
        auto* directionalLight = new Entity();
        directionalLight->setEngine(engine());
        _sun = static_cast<LightComponent*>(directionalLight->addComponent<LightComponent>());
        _sun->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        _sun->setColor(Color(1.0f, 0.94f, 0.85f, 1.0f));
        _sun->setCastShadows(true);
        _sun->setLuminance(_sunLuminance);
        _sun->setShadowBias(0.2f);
        _sun->setShadowNormalBias(0.05f);
        _sun->setShadowResolution(2048);
        directionalLight->setLocalEulerAngles(45.0f, 35.0f, 0.0f);
        root()->addChild(directionalLight);

        // ------ Omni light ------
        _omniEntity = new Entity();
        _omniEntity->setEngine(engine());
        _omni = static_cast<LightComponent*>(_omniEntity->addComponent<LightComponent>());
        _omni->setType(LightType::LIGHTTYPE_OMNI);
        _omni->setRange(7.0f);
        _omni->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
        _omni->setColor(Color(1.0f, 0.4f, 0.75f, 1.0f));
        _omni->setCastShadows(false);
        _omni->setLuminance(_pointLuminance);
        _omni->setShadowBias(0.2f);
        _omni->setShadowNormalBias(0.05f);
        _omni->setShadowResolution(2048);
        _omniEntity->setLocalPosition(-0.5f, 1.2f, -5.0f);
        root()->addChild(_omniEntity);

        // ------ Spot light ------
        _spotEntity = new Entity();
        _spotEntity->setEngine(engine());
        _spot = static_cast<LightComponent*>(_spotEntity->addComponent<LightComponent>());
        _spot->setType(LightType::LIGHTTYPE_SPOT);
        _spot->setRange(8.0f);
        _spot->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
        _spot->setColor(Color(0.55f, 0.75f, 1.0f, 1.0f));
        _spot->setCastShadows(false);
        _spot->setLuminance(_spotLuminance);
        _spot->setShadowBias(0.2f);
        _spot->setShadowNormalBias(0.05f);
        _spot->setShadowResolution(2048);
        _spot->setOuterConeAngle(_spotAngle);
        _spot->setInnerConeAngle(0.0f);
        root()->addChild(_spotEntity);
        _spotEntity->setLocalPosition(10.0f, 1.5f, -5.0f);
        // lookAt points the forward axis; a spot light shines along its local -Y axis.
        _spotEntity->lookAt(Vector3(9.5f, 0.0f, 0.0f));
        _spotEntity->rotateLocal(90.0f, 0.0f, 0.0f);

        // ------ Rect area light ------
        const Color areaColor(1.0f, 0.72f, 0.38f, 1.0f);
        _areaEntity = new Entity();
        _areaEntity->setEngine(engine());
        _rect = static_cast<LightComponent*>(_areaEntity->addComponent<LightComponent>());
        _rect->setType(LightType::LIGHTTYPE_SPOT);
        _rect->setShape(LightShape::LIGHTSHAPE_RECT);
        _rect->setColor(areaColor);
        _rect->setRange(9999.0f);
        _rect->setLuminance(_rectLuminance);
        _rect->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
        _rect->setInnerConeAngle(80.0f);
        _rect->setOuterConeAngle(85.0f);
        _rect->setShadowNormalBias(0.1f);
        _areaEntity->setLocalScale(4.0f, 1.0f, 5.0f);
        _areaEntity->setLocalEulerAngles(70.0f, 180.0f, 0.0f);
        _areaEntity->setLocalPosition(5.0f, 3.0f, -5.0f);

        // Emissive material that is the light source colour.
        _brightMaterial = std::make_shared<StandardMaterial>();
        _brightMaterial->setEmissive(areaColor);
        _brightMaterial->setEmissiveIntensity(_rect->luminance());
        _brightMaterial->setUseLighting(false);
        _brightMaterial->setCullMode(CullMode::CULLFACE_NONE);

        // Primitive shape that matches the light source shape, a child of the light, so turning
        // the light off hides it too.
        auto* brightShape = new Entity();
        brightShape->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(brightShape->addComponent<RenderComponent>())) {
            render->setMaterial(_brightMaterial.get());
            render->setType("plane");
            render->setCastShadows(false);
        }
        _areaEntity->addChild(brightShape);
        root()->addChild(_areaEntity);

        // ------ Camera ------
        auto* camera = createCamera(Vector3(11.0f, 4.0f, 13.0f));
        CameraComponent* cameraComponent = camera->findComponent<CameraComponent>();
        if (cameraComponent) {
            _camera = cameraComponent->camera();
        }
        if (!_camera) {
            spdlog::error("Camera entity has no camera");
            return false;
        }
        _camera->setClearColor(kBackgroundColor);
        _camera->setFov(40.0f);
        cameraComponent->requestSceneColorMap(true);
        applyCamera();

        const Vector3 focusPoint(6.5f, 1.5f, -1.0f);
        camera->lookAt(focusPoint);
        if (auto* controls = addOrbitControls(camera, focusPoint)) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(1.0f, 400.0f));
            controls->storeResetState();
        }

        // Tone mapping and vignette, through the camera frame.
        cameraComponent->setToneMapping(TONEMAP_ACES);
        auto& rendering = cameraComponent->rendering();
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.6f;

        // Draw the shape of each light, in the colour of the light.
        const auto gizmoLayer = Gizmo::createLayer(engine(), "Helpers", INT_MAX, true);
        _lightGizmo = std::make_unique<LightGizmo>(cameraComponent, gizmoLayer);
        _lightGizmo->handles = false;
        _lightGizmo->useLightColor = true;
        _lightGizmo->attach(std::vector<GraphNode*>{_omniEntity, _spotEntity});

        applyPhysicalUnits();
        applySky();

        spdlog::info("Keys: Z/X/C rect/point/spot on/off, 1/2 rect, 3/4 point, 5/6 spot luminance, "
                     "7/8 spot angle, 9/0 aperture, -/= shutter, [/] ISO, ;/' sky, ,/. sun luminance, "
                     "Space animate, P physical units, K skylight, R reset camera, Esc quit");
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

    void destroy() override
    {
        _lightGizmo.reset();
    }

private:
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

        if (keyboard->wasPressed(Key::Z)) {
            _rectEnabled = !_rectEnabled;
            _areaEntity->setEnabled(_rectEnabled);
            changed = true;
        }
        if (keyboard->wasPressed(Key::X)) {
            _pointEnabled = !_pointEnabled;
            _omniEntity->setEnabled(_pointEnabled);
            changed = true;
        }
        if (keyboard->wasPressed(Key::C)) {
            _spotEnabled = !_spotEnabled;
            _spotEntity->setEnabled(_spotEnabled);
            changed = true;
        }

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
            _shutter = std::clamp(_shutter, 1.0f, kShutterMax);
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
        // Authored gamma space: decodes to the colour times the luminance, or to the colour
        // alone without units.
        const float scale = _physicalUnits ? kAmbientLuminance : 1.0f;
        const auto ambient = [scale](const float channel) {
            return linearToGamma(gammaToLinear(channel) * scale);
        };
        scene()->setAmbientLight(ambient(kAmbientColor[0]), ambient(kAmbientColor[1]),
            ambient(kAmbientColor[2]));
    }

    // Without the skylight the scene has no environment, and the ambient colour lights it.
    void applySky() const
    {
        scene()->setEnvAtlas(_sky ? _helipad : nullptr);
    }

    void logState() const
    {
        spdlog::info("Rect {} {:.0f}, point {} {:.0f}, spot {} {:.0f} (angle {:.0f}), sun {:.0f}, sky {:.0f} | "
                     "f/{:.1f}, 1/{:.1f} s, ISO {:.0f}, exposure {:.3g} | animate {}, physical {}, skylight {}",
            _rectEnabled ? "on" : "off", _rectLuminance, _pointEnabled ? "on" : "off", _pointLuminance,
            _spotEnabled ? "on" : "off", _spotLuminance, _spotAngle, _sunLuminance, _skyLuminance,
            _aperture, _shutter, _sensitivity, scene()->exposureFor(_camera),
            _animate ? "on" : "off", _physicalUnits ? "on" : "off", _sky ? "on" : "off");
    }

    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<Asset> _sheenAsset;

    Texture* _helipad = nullptr;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::shared_ptr<StandardMaterial> _brightMaterial;
    std::unique_ptr<LightGizmo> _lightGizmo;

    LightComponent* _sun = nullptr;
    LightComponent* _omni = nullptr;
    LightComponent* _spot = nullptr;
    LightComponent* _rect = nullptr;
    Entity* _omniEntity = nullptr;
    Entity* _spotEntity = nullptr;
    Entity* _areaEntity = nullptr;
    Camera* _camera = nullptr;

    // The control panel's initial settings.
    float _sunLuminance = 100000.0f;
    float _skyLuminance = 6000.0f;
    bool _spotEnabled = true;
    float _spotLuminance = 600000.0f;
    float _spotAngle = 30.0f;
    bool _pointEnabled = true;
    float _pointLuminance = 1200000.0f;
    bool _rectEnabled = true;
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
