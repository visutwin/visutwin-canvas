// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/sky.
//
// A marble statue at the origin, lit only by the environment, inside a sky projected from an
// HDRI. Each HDRI is turned into a high-resolution skybox cube map for the sky and an env
// atlas for the lighting. Three presets: "Street Dome" (the default), the wide-street HDRI on
// a dome (scale 200, tripod at height 0.05); "Street Infinite", the same HDRI at infinity;
// and "Room", the small-empty-room HDRI on a box (scale 44 x 24 x 28, tripod 0.6, turned 50
// degrees). All of them at exposure 0.7. The camera (far 500, fov 60, ACES) orbits the
// statue from (-4, 5, 22). The camera frame's colour enhance (shadows, highlights,
// midtones, vibrance, dehaze) is available, off and at zero by default.
//
// Keys stand in for upstream's control panel:
//   1 / 2 / 3   Preset: Street Dome / Street Infinite / Room
//   T           Type: cycle Infinite, Box, Dome
//   - / =       Exposure -/+ 0.05, 0..3
//   [ / ]       Rotation -/+ 10 degrees, 0..360
//   Down / Up   Tripod Y -/+ 0.05, 0..1
//   C           Color Enhance on/off
//   Q / A       shadows +/- 0.25, -3..3
//   W / S       highlights +/- 0.25, -3..3
//   E / D       midtones +/- 0.1, -1..1
//   Y / H       vibrance +/- 0.1, -1..1
//   U / J       dehaze +/- 0.1, -1..1
// A preset resets type, scale, position, tripod, exposure and rotation, and keeps the colour
// enhance settings.
//
// HDRIs: "Wide Street 02" and "Small Empty Room 2" by Poly Haven, CC BY 4.0.
//
// DEVIATION: upstream's Scale and Position vector fields have no keys here; the sky's scale
// and position are the presets' own.
// DEVIATION: upstream asks for an HDR display and renders untonemapped on one; there is no
// HDR output here, so the scene takes upstream's SDR branch, ACES.
// DEVIATION: upstream renders through a camera frame whose effects are all off; here the
// camera renders directly until colour enhance changes the image, which the camera frame
// then applies. The two images agree.
// DEVIATION: upstream's orbitCamera script (inertia 0.2, distanceMax 500, frameOnStart off)
// is the examples' CameraControls in orbit mode around the statue's bounds centre, at the
// authored camera position's distance, zoom capped at 500. Its damping is CameraControls'
// own.
//
#include <algorithm>
#include <array>
#include <memory>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"

using namespace visutwin::canvas;

namespace
{
    enum class Hdri { Street = 0, Room = 1 };

    struct SkyPreset
    {
        const char* name;
        Hdri hdri;
        int type;
        Vector3 scale;
        Vector3 position;
        float tripodY;
        float exposure;
        float rotation;
    };

    // Skydome presets
    const std::array<SkyPreset, 3> kPresets = {{
        {"Street Dome", Hdri::Street, SKYTYPE_DOME, Vector3(200.0f, 200.0f, 200.0f), Vector3(0.0f, 0.0f, 0.0f),
            0.05f, 0.7f, 0.0f},
        {"Street Infinite", Hdri::Street, SKYTYPE_INFINITE, Vector3(1.0f, 1.0f, 1.0f), Vector3(0.0f, 0.0f, 0.0f),
            0.0f, 0.7f, 0.0f},
        {"Room", Hdri::Room, SKYTYPE_BOX, Vector3(44.0f, 24.0f, 28.0f), Vector3(0.0f, 0.0f, 0.0f),
            0.6f, 0.7f, 50.0f},
    }};
}

class SkyExample final: public ExampleApp
{
public:
    // The camera frame renders the scene single-sampled; the back buffer's multisampling
    // would only antialias the direct path, so the two would differ.
    SkyExample(): ExampleApp({.title = "Sky", .antialias = false}) {}

protected:
    bool create() override
    {
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _hdriAssets[0] = std::make_unique<Asset>("hdri", AssetType::TEXTURE, assetPath("hdri/wide-street.hdr"),
            AssetData{.mipmaps = false});
        _hdriAssets[1] = std::make_unique<Asset>("hdri", AssetType::TEXTURE, assetPath("hdri/empty-room.hdr"),
            AssetData{.mipmaps = false});
        auto* statue = _statueAsset->resourceAs<ContainerResource>();
        if (!statue || !_hdriAssets[0]->resourceAs<Texture>() || !_hdriAssets[1]->resourceAs<Texture>()) {
            spdlog::error("Failed to load the statue or the wide-street / empty-room HDRIs");
            return false;
        }

        // Add an instance of the statue
        _statue = statue->instantiateRenderEntity();
        _statue->setEngine(engine());
        root()->addChild(_statue);

        // Create an Entity with a camera component
        auto* camera = createCamera(Vector3(-4.0f, 5.0f, 22.0f));
        camera->lookAt(0.0f, 0.0f, 1.0f);
        _cameraComponent = camera->findComponent<CameraComponent>();
        if (!_cameraComponent || !_cameraComponent->camera()) {
            spdlog::error("Failed to create the camera");
            return false;
        }
        _cameraComponent->camera()->setFarClip(500.0f);
        _cameraComponent->camera()->setFov(60.0f);
        _cameraComponent->setToneMapping(TONEMAP_ACES);

        // Orbit the statue
        if (auto* controls = addOrbitControls(camera, entityBounds(_statue).center())) {
            controls->setZoomRange(Vector2(0.0f, 500.0f));
            controls->storeResetState();
        }

        // Apply initial preset
        applyPreset(0);
        applyColorEnhance();

        spdlog::info("Keys: 1/2/3 preset, T type, - = exposure, [ ] rotation, Down/Up tripod Y, "
            "C color enhance, Q/A W/S E/D Y/H U/J shadows/highlights/midtones/vibrance/dehaze");
        return true;
    }

    void update(float /*dt*/) override
    {
        handleKeys();
    }

    void destroy() override
    {
        // The scene borrows the skybox and the env atlas; it is torn down after this
        if (scene()) {
            scene()->setSkybox(nullptr);
            scene()->setEnvAtlas(nullptr);
        }
    }

private:
    // Apply hdri texture. Each HDRI is converted once and kept, so switching presets never
    // frees a texture a frame in flight may still sample.
    void applyHdri(const Hdri hdri)
    {
        const auto index = static_cast<size_t>(hdri);
        if (!_skyboxes[index]) {
            Texture* source = _hdriAssets[index]->resourceAs<Texture>();

            // Convert it to high resolution cubemap for the skybox
            _skyboxes[index].reset(EnvLighting::generateSkyboxCubemap(device().get(), source));

            // Generate env-atlas texture for the lighting
            _envAtlases[index].reset(EnvLighting::generateAtlas(device().get(), source));
        }
        scene()->setSkybox(_skyboxes[index].get());
        scene()->setEnvAtlas(_envAtlases[index].get());
    }

    void applyPreset(const size_t index)
    {
        const SkyPreset& preset = kPresets[index];
        applyHdri(preset.hdri);
        _type = preset.type;
        _scale = preset.scale;
        _position = preset.position;
        _tripodY = preset.tripodY;
        _exposure = preset.exposure;
        _rotation = preset.rotation;
        applySky();
        spdlog::info("Preset: {}", preset.name);
    }

    void applySky()
    {
        scene()->setSkyType(_type);
        scene()->sky()->node()->setLocalScale(_scale);
        scene()->sky()->node()->setLocalPosition(_position);
        scene()->sky()->setCenter(Vector3(0.0f, _tripodY, 0.0f));
        scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.0f, _rotation, 0.0f));
        scene()->setExposure(_exposure);
    }

    void applyColorEnhance()
    {
        auto rendering = _cameraComponent->rendering();
        rendering.colorEnhanceShadows = _enhance ? _shadows : 0.0f;
        rendering.colorEnhanceHighlights = _enhance ? _highlights : 0.0f;
        rendering.colorEnhanceMidtones = _enhance ? _midtones : 0.0f;
        rendering.colorEnhanceVibrance = _enhance ? _vibrance : 0.0f;
        rendering.colorEnhanceDehaze = _enhance ? _dehaze : 0.0f;
        _cameraComponent->setRendering(rendering);
    }

    static void step(float& value, const float delta, const float low, const float high)
    {
        value = std::clamp(value + delta, low, high);
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }

        // Preset
        if (keyboard->wasPressed(Key::Digit1)) {
            applyPreset(0);
        } else if (keyboard->wasPressed(Key::Digit2)) {
            applyPreset(1);
        } else if (keyboard->wasPressed(Key::Digit3)) {
            applyPreset(2);
        }

        // Individual sky settings
        bool skyChanged = true;
        if (keyboard->wasPressed(Key::T)) {
            _type = _type == SKYTYPE_INFINITE ? SKYTYPE_BOX : (_type == SKYTYPE_BOX ? SKYTYPE_DOME : SKYTYPE_INFINITE);
        } else if (keyboard->wasPressed(Key::Minus)) {
            step(_exposure, -0.05f, 0.0f, 3.0f);
        } else if (keyboard->wasPressed(Key::Equals)) {
            step(_exposure, 0.05f, 0.0f, 3.0f);
        } else if (keyboard->wasPressed(Key::LeftBracket)) {
            step(_rotation, -10.0f, 0.0f, 360.0f);
        } else if (keyboard->wasPressed(Key::RightBracket)) {
            step(_rotation, 10.0f, 0.0f, 360.0f);
        } else if (keyboard->wasPressed(Key::Down)) {
            step(_tripodY, -0.05f, 0.0f, 1.0f);
        } else if (keyboard->wasPressed(Key::Up)) {
            step(_tripodY, 0.05f, 0.0f, 1.0f);
        } else {
            skyChanged = false;
        }
        if (skyChanged) {
            applySky();
            spdlog::info("Sky: type {} exposure {:.2f} rotation {:.0f} tripod Y {:.2f}",
                _type == SKYTYPE_INFINITE ? "infinite" : (_type == SKYTYPE_BOX ? "box" : "dome"),
                _exposure, _rotation, _tripodY);
        }

        // ColorEnhance
        bool enhanceChanged = true;
        if (keyboard->wasPressed(Key::C)) {
            _enhance = !_enhance;
        } else if (keyboard->wasPressed(Key::Q)) {
            step(_shadows, 0.25f, -3.0f, 3.0f);
        } else if (keyboard->wasPressed(Key::A)) {
            step(_shadows, -0.25f, -3.0f, 3.0f);
        } else if (keyboard->wasPressed(Key::W)) {
            step(_highlights, 0.25f, -3.0f, 3.0f);
        } else if (keyboard->wasPressed(Key::S)) {
            step(_highlights, -0.25f, -3.0f, 3.0f);
        } else if (keyboard->wasPressed(Key::E)) {
            step(_midtones, 0.1f, -1.0f, 1.0f);
        } else if (keyboard->wasPressed(Key::D)) {
            step(_midtones, -0.1f, -1.0f, 1.0f);
        } else if (keyboard->wasPressed(Key::Y)) {
            step(_vibrance, 0.1f, -1.0f, 1.0f);
        } else if (keyboard->wasPressed(Key::H)) {
            step(_vibrance, -0.1f, -1.0f, 1.0f);
        } else if (keyboard->wasPressed(Key::U)) {
            step(_dehaze, 0.1f, -1.0f, 1.0f);
        } else if (keyboard->wasPressed(Key::J)) {
            step(_dehaze, -0.1f, -1.0f, 1.0f);
        } else {
            enhanceChanged = false;
        }
        if (enhanceChanged) {
            applyColorEnhance();
            spdlog::info("Color enhance {}: shadows {:.2f} highlights {:.2f} midtones {:.2f} vibrance {:.2f} "
                "dehaze {:.2f}", _enhance ? "on" : "off", _shadows, _highlights, _midtones, _vibrance, _dehaze);
        }
    }

    std::unique_ptr<Asset> _statueAsset;
    std::array<std::unique_ptr<Asset>, 2> _hdriAssets;
    std::array<std::unique_ptr<Texture>, 2> _skyboxes;
    std::array<std::unique_ptr<Texture>, 2> _envAtlases;

    Entity* _statue = nullptr;
    CameraComponent* _cameraComponent = nullptr;

    // Sky settings, from the preset
    int _type = SKYTYPE_DOME;
    Vector3 _scale = Vector3(1.0f, 1.0f, 1.0f);
    Vector3 _position = Vector3(0.0f, 0.0f, 0.0f);
    float _tripodY = 0.0f;
    float _exposure = 0.7f;
    float _rotation = 0.0f;

    // Initial colorEnhance values
    bool _enhance = false;
    float _shadows = 0.0f;
    float _highlights = 0.0f;
    float _midtones = 0.0f;
    float _vibrance = 0.0f;
    float _dehaze = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(SkyExample)
