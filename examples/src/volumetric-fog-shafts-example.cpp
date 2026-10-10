// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream camera-frame/volumetric-fog-shafts.
//
// Shafts of light cast through the holes of a slowly tumbling carbon 60 sphere (a
// cage of hexagons and pentagons, scaled to a radius of 90) by the volumetric fog.
// Two shadowed spot lights, pale blue and warm orange, orbit the sphere at its own
// height 240 units out and shine straight through it (range 600, linear falloff,
// cones 14 / 23 degrees); a shadowed warm omni light at its centre, marked by a small
// emissive sphere, radiates out through the holes with a fifth of the scattering.
// There is no environment lighting and no directional light: the local lights alone
// light the sphere and, through the clustered shadow atlas (4096) and cookie atlas
// (2048), the fog. The camera frame renders with TAA, neutral tone mapping,
// sharpening and a subtle bloom, and the fog has no height falloff and a faint blue
// ambient term.
//
// Keys stand in for upstream's control panel:
//   Lights:  1 spot lights   2 omni light   3 shadows   4 video cookie   5 animate
//            - / = scattering (local intensity) -/+
//   Fog:     F enabled       T TAA
//            [ / ] density -/+          ; / ' extinction -/+
//            , / . max distance -/+     Z / X anisotropy -/+
//            N / M steps -/+            J / K resolution scale -/+
//   R reset the camera, F1 statistics, Esc quit; drag to orbit, wheel to zoom.
//
// DEVIATIONS:
// - The optional spot-light cookie is a video upstream (a 1280x720 MP4 played through
//   an HTML video element and uploaded every frame). This engine has no video
//   decoding and the repository carries no video, so the cookie is an animated 320x180
//   test pattern generated on the CPU, advanced by the frame time and uploaded every
//   frame while the cookie is on.
// - upstream caps the pixel ratio at 2; the examples render at one pixel per point.
//
// @credit CARBON 60 SPHERE by Random13, CC BY 4.0
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/texture.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kPi = std::numbers::pi_v<float>;

    // The model is roughly 52 units in radius; it is scaled to the size the scene is
    // built around.
    constexpr float kSphereModelRadius = 52.0f;
    constexpr float kSphereRadius = 90.0f;

    // The spot lights orbit the sphere at its own height, so their centre rays run
    // parallel to the ground and shine straight through the cage.
    constexpr float kSpotOrbitRadius = 240.0f;

    // The video stand-in keeps the video's 16:9.
    constexpr uint32_t kVideoWidth = 320;
    constexpr uint32_t kVideoHeight = 180;
}

class VolumetricFogShaftsExample final: public ExampleApp
{
public:
    VolumetricFogShaftsExample()
        : ExampleApp({.title = "Volumetric Fog Shafts"}) {}

protected:
    bool create() override
    {
        // The lights sample both the shadow and the cookie atlas of the clustered lighting.
        // A large shadow atlas keeps the many small holes of the sphere sharp in the shafts.
        auto& lighting = scene()->lighting();
        lighting.shadowAtlasResolution = 4096;
        lighting.cookieAtlasResolution = 2048;
        lighting.cookiesEnabled = true;

        // No environment lighting: the sphere is lit by the local lights alone.
        scene()->setAmbientLight(0.02f, 0.022f, 0.03f);

        // The carbon 60 sphere, a cage the light shines through.
        _sphereAsset = std::make_unique<Asset>("carbon-sphere", AssetType::CONTAINER,
            assetPath("models/carbon-sphere.glb"));
        ContainerResource* container = _sphereAsset->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Failed to load carbon-sphere.glb");
            return false;
        }
        _sphere = container->instantiateRenderEntity();
        if (!_sphere) {
            spdlog::error("Failed to instantiate carbon-sphere.glb");
            return false;
        }
        _sphere->setEngine(engine());
        for (auto* render : _sphere->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
        }
        const float sphereScale = kSphereRadius / kSphereModelRadius;
        _sphere->setLocalScale(sphereScale, sphereScale, sphereScale);
        root()->addChild(_sphere);

        // The video stand-in, an optional cookie of the spot lights, so that the pattern
        // inside the shafts animates.
        TextureOptions cookieOptions;
        cookieOptions.name = "videoCookie";
        cookieOptions.width = kVideoWidth;
        cookieOptions.height = kVideoHeight;
        cookieOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        cookieOptions.mipmaps = false;
        cookieOptions.minFilter = FilterMode::FILTER_LINEAR;
        cookieOptions.magFilter = FilterMode::FILTER_LINEAR;
        _videoCookie = std::make_shared<Texture>(device().get(), cookieOptions);
        _videoCookie->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _videoCookie->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _videoPixels.resize(static_cast<size_t>(kVideoWidth) * kVideoHeight * 4);
        updateVideoCookie(0.0f);

        // Two spot lights orbiting the sphere, both aimed at its centre. The cones are
        // wide enough to cover the whole sphere, so the holes shape the shafts.
        const std::array<Color, 2> spotColors = {Color(0.5f, 0.8f, 1.0f, 1.0f), Color(1.0f, 0.75f, 0.5f, 1.0f)};
        for (size_t index = 0; index < spotColors.size(); ++index) {
            auto* spot = new Entity();
            spot->setName("Spot-" + std::to_string(index));
            spot->setEngine(engine());
            auto* light = static_cast<LightComponent*>(spot->addComponent<LightComponent>());
            light->setType(LightType::LIGHTTYPE_SPOT);
            light->setColor(spotColors[index]);
            light->setIntensity(15.0f);
            light->setRange(600.0f);
            light->setFalloffMode(LightFalloff::LIGHTFALLOFF_LINEAR);
            light->setInnerConeAngle(14.0f);
            light->setOuterConeAngle(23.0f);
            light->setCastShadows(true);
            light->setShadowBias(0.06f);
            light->setShadowNormalBias(0.03f);
            light->setShadowType(SHADOW_PCF3_32F);
            light->setCookie(_videoCookie.get());
            light->setCookieChannel(CookieChannel::COOKIE_CHANNEL_RGB);
            light->setCookieIntensity(0.0f);
            root()->addChild(spot);
            _spots.push_back(spot);
            _spotLights.push_back(light);
        }

        // A single omni light at the centre of the sphere, its light radiating out through
        // the holes; it is given no cookie. Spreading its energy over every direction rather
        // than into a narrow cone, it needs a fraction of the spot lights' scattering: much
        // above 0.2 the glow right around the light brightens faster than the distant
        // shafts, which then lose definition.
        _omni = new Entity();
        _omni->setName("Omni");
        _omni->setEngine(engine());
        _omniLight = static_cast<LightComponent*>(_omni->addComponent<LightComponent>());
        _omniLight->setType(LightType::LIGHTTYPE_OMNI);
        _omniLight->setColor(Color(1.0f, 0.8f, 0.55f, 1.0f));
        _omniLight->setIntensity(5.0f);
        _omniLight->setRange(320.0f);
        _omniLight->setFalloffMode(LightFalloff::LIGHTFALLOFF_LINEAR);
        _omniLight->setCastShadows(true);
        _omniLight->setShadowBias(0.06f);
        _omniLight->setShadowNormalBias(0.03f);
        _omniLight->setShadowType(SHADOW_PCF3_32F);
        _omniLight->setVolumetricScattering(0.2f);
        _omni->setLocalPosition(0.0f, 0.0f, 0.0f);
        root()->addChild(_omni);

        // A small emissive sphere marking the omni light.
        _bulbMaterial = std::make_shared<StandardMaterial>();
        _bulbMaterial->setEmissive(Color(1.0f, 0.8f, 0.55f, 1.0f));
        _bulbMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        auto* bulb = new Entity();
        bulb->setName("Bulb");
        bulb->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(bulb->addComponent<RenderComponent>())) {
            render->setMaterial(_bulbMaterial.get());
            render->setType("sphere");
            render->setCastShadows(false);
        }
        bulb->setLocalScale(6.0f, 6.0f, 6.0f);
        _omni->addChild(bulb);

        // The camera, orbiting the sphere's centre from its authored position.
        auto* camera = createCamera(Vector3(-400.0f, 150.0f, 440.0f));
        _cameraComponent = camera->findComponent<CameraComponent>();
        if (_cameraComponent && _cameraComponent->camera()) {
            _cameraComponent->camera()->setClearColor(Color(0.01f, 0.012f, 0.02f, 1.0f));
            _cameraComponent->camera()->setNearClip(1.0f);
            _cameraComponent->camera()->setFarClip(2000.0f);
        }
        const Vector3 focusPoint(0.0f, 0.0f, 0.0f);
        camera->lookAt(focusPoint);
        if (auto* controls = addOrbitControls(camera, focusPoint)) {
            controls->setPitchRange(Vector2(-80.0f, 25.0f));
            controls->setZoomRange(Vector2(40.0f, 900.0f));
            controls->storeResetState();
        }

        // The camera frame: TAA and the volumetric fog. No directional light reaches the
        // fog; the local lights light it on their own.
        if (_cameraComponent) {
            _cameraComponent->setToneMapping(TONEMAP_NEUTRAL);
            auto rendering = _cameraComponent->rendering();
            rendering.sharpness = 0.5f;
            rendering.bloomIntensity = 0.015f;
            _cameraComponent->setRendering(rendering);

            auto& fog = _cameraComponent->volumetricFog();
            fog.tint[0] = 0.9f;
            fog.tint[1] = 0.94f;
            fog.tint[2] = 1.0f;
            fog.heightBase = 0.0f;
            fog.heightFalloff = 0.0f;
            fog.ambientColor[0] = 0.3f;
            fog.ambientColor[1] = 0.45f;
            fog.ambientColor[2] = 0.8f;
            fog.ambientIntensity = 0.004f;

            // With no directional light the main march only accumulates the flat ambient
            // term and the extinction, so a low number of steps is enough for it.
            fog.steps = 10;
        }
        applySettings();

        // The lights take their first pose before the first frame renders.
        updateLights();

        spdlog::info("Keys: 1 spot, 2 omni, 3 shadows, 4 cookie, 5 animate, - = scattering | "
                     "F fog, T TAA, [ ] density, ; ' extinction, , . max distance, Z X anisotropy, "
                     "N M steps, J K scale");
        return true;
    }

    void update(const float dt) override
    {
        handleKeys();

        if (_animate) {
            _lightTime += dt;
        }
        updateLights();

        // The video plays on whether or not the lights animate; its next frame is uploaded
        // while the spot lights project it.
        _videoTime += dt;
        if (_cookie) {
            updateVideoCookie(_videoTime);
        }
    }

private:
    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }

        bool changed = false;
        const auto toggle = [&](const Key key, bool& value) {
            if (keyboard->wasPressed(key)) {
                value = !value;
                changed = true;
            }
        };
        const auto step = [&](const Key down, const Key up, auto& value, const auto delta,
            const auto minimum, const auto maximum) {
            if (keyboard->wasPressed(down)) {
                value = std::clamp(value - delta, minimum, maximum);
                changed = true;
            }
            if (keyboard->wasPressed(up)) {
                value = std::clamp(value + delta, minimum, maximum);
                changed = true;
            }
        };

        // Lights panel.
        toggle(Key::Digit1, _spot);
        toggle(Key::Digit2, _omniOn);
        toggle(Key::Digit3, _shadows);
        toggle(Key::Digit4, _cookie);
        toggle(Key::Digit5, _animate);
        step(Key::Minus, Key::Equals, _localIntensity, 2.0f, 0.0f, 60.0f);

        // Volumetric fog panel, the slider ranges upstream's.
        toggle(Key::F, _fogEnabled);
        toggle(Key::T, _taa);
        step(Key::LeftBracket, Key::RightBracket, _density, 0.00025f, 0.0f, 0.006f);
        step(Key::Semicolon, Key::Apostrophe, _extinction, 0.1f, 0.0f, 2.0f);
        step(Key::Comma, Key::Period, _maxDistance, 100.0f, 200.0f, 4000.0f);
        step(Key::Z, Key::X, _anisotropy, 0.05f, 0.0f, 0.95f);
        step(Key::N, Key::M, _localSteps, 2, 2, 64);
        step(Key::J, Key::K, _scale, 0.05f, 0.25f, 1.0f);

        if (changed) {
            applySettings();
        }
    }

    // The settings, from the control panel's defaults.
    void applySettings()
    {
        if (_cameraComponent) {
            _cameraComponent->setTaaEnabled(_taa);
            auto& fog = _cameraComponent->volumetricFog();

            // Upstream's fog contributes nothing at all, not even its ambient term or its
            // extinction, while no light feeds it; with no directional light in the scene
            // that is whenever both local light types are off. This engine's fog would still
            // march them, so it is switched off then.
            fog.enabled = _fogEnabled && (_spot || _omniOn);
            fog.density = _density;
            fog.anisotropy = _anisotropy;
            fog.extinction = _extinction;
            fog.maxDistance = _maxDistance;
            fog.localSteps = _localSteps;
            fog.localIntensity = _localIntensity;
            fog.scale = _scale;

            // Each light type scatters light in the fog only while it is enabled.
            fog.localSpotLights = _spot;
            fog.localOmniLights = _omniOn;
        }

        for (size_t index = 0; index < _spots.size(); ++index) {
            _spots[index]->setEnabled(_spot);
            _spotLights[index]->setCastShadows(_shadows);
            _spotLights[index]->setCookieIntensity(_cookie ? 1.0f : 0.0f);
        }
        if (_omni && _omniLight) {
            _omni->setEnabled(_omniOn);
            _omniLight->setCastShadows(_shadows);
        }

        spdlog::info("spot {} omni {} scattering {:.0f} shadows {} cookie {} animate {} | fog {} density {:.5f} "
                     "extinction {:.2f} max distance {:.0f} anisotropy {:.2f} steps {} scale {:.2f} TAA {}",
            _spot, _omniOn, _localIntensity, _shadows, _cookie, _animate, _fogEnabled, _density, _extinction,
            _maxDistance, _anisotropy, _localSteps, _scale, _taa);
    }

    void updateLights()
    {
        // Slowly tumble the sphere, which sweeps its holes across the light.
        if (_sphere) {
            _sphere->setLocalEulerAngles(_lightTime * 3.0f, _lightTime * 7.0f, 0.0f);
        }

        // Orbit the spot lights around the sphere, both aiming at its centre. A spot shines
        // down its negative Y axis, so the entity is turned after aiming it.
        for (size_t index = 0; index < _spots.size(); ++index) {
            const float angle = (static_cast<float>(index) / static_cast<float>(_spots.size())) * kPi * 2.0f +
                _lightTime * 0.25f;
            _spots[index]->setLocalPosition(kSpotOrbitRadius * std::sin(angle), 0.0f,
                kSpotOrbitRadius * std::cos(angle));
            _spots[index]->lookAt(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
            _spots[index]->rotateLocal(90.0f, 0.0f, 0.0f);
        }
    }

    // The video stand-in: seven colour bars scrolling sideways, with a bright disc
    // circling the middle of the frame.
    void updateVideoCookie(const float time)
    {
        static constexpr std::array<std::array<uint8_t, 3>, 7> kBars = {{
            {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
            {191, 0, 191}, {191, 0, 0}, {0, 0, 191},
        }};
        const float width = static_cast<float>(kVideoWidth);
        const float height = static_cast<float>(kVideoHeight);
        const float scroll = time * 0.1f;
        const float discX = width * (0.5f + 0.3f * std::cos(time * 1.3f));
        const float discY = height * (0.5f + 0.3f * std::sin(time * 1.3f));
        const float discRadius = height * 0.18f;

        for (uint32_t y = 0; y < kVideoHeight; ++y) {
            for (uint32_t x = 0; x < kVideoWidth; ++x) {
                const float u = static_cast<float>(x) / width + scroll;
                const float wrapped = u - std::floor(u);
                const auto bar = std::min(static_cast<size_t>(wrapped * 7.0f), kBars.size() - 1);
                const auto& rgb = kBars[bar];

                const float ddx = static_cast<float>(x) - discX;
                const float ddy = static_cast<float>(y) - discY;
                const bool inDisc = ddx * ddx + ddy * ddy < discRadius * discRadius;

                const size_t i = (static_cast<size_t>(y) * kVideoWidth + x) * 4;
                _videoPixels[i] = inDisc ? 255 : rgb[0];
                _videoPixels[i + 1] = inDisc ? 255 : rgb[1];
                _videoPixels[i + 2] = inDisc ? 255 : rgb[2];
                _videoPixels[i + 3] = 255;
            }
        }
        _videoCookie->setLevelData(0, _videoPixels.data(), _videoPixels.size());
        _videoCookie->upload();
    }

    std::unique_ptr<Asset> _sphereAsset;
    std::shared_ptr<StandardMaterial> _bulbMaterial;
    std::shared_ptr<Texture> _videoCookie;
    std::vector<uint8_t> _videoPixels;
    Entity* _sphere = nullptr;
    std::vector<Entity*> _spots;
    std::vector<LightComponent*> _spotLights;
    Entity* _omni = nullptr;
    LightComponent* _omniLight = nullptr;
    CameraComponent* _cameraComponent = nullptr;

    // Lights panel.
    bool _spot = true;
    bool _omniOn = false;
    float _localIntensity = 22.0f;
    bool _shadows = true;
    bool _cookie = false;
    bool _animate = true;

    // Volumetric fog panel. The shadow map texels of the cage are extruded along each
    // beam, and enough steps are needed for the march to average them out rather than
    // alias on them.
    bool _fogEnabled = true;
    float _density = 0.0015f;
    float _extinction = 1.0f;
    float _maxDistance = 2500.0f;
    float _anisotropy = 0.6f;
    int _localSteps = 32;
    float _scale = 0.5f;
    bool _taa = true;

    float _lightTime = 0.0f;
    float _videoTime = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(VolumetricFogShaftsExample)
