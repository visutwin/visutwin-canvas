// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// Port of upstream xr/vr-basic.
//
// The gallery model under the table-mountain environment (exposure 0.5, sky at mip 0) and a
// spot light 10 units up, seen from a camera at (1.35, 2.45, 3) looking at (0, 1.25, 0).
// Starting the session hands the camera to the headset: it renders one view per eye, in the
// local-floor space, and the window mirrors the left eye.
//
// DEVIATION: upstream starts the session on a click or a touch and ends it on Escape; here X
// starts and ends it (a click drives nothing else in this window, and Escape closes it).
// DEVIATION: the session runs on OpenXR when a runtime with a headset is there, and otherwise
// on the engine's simulated headset, so the example shows stereo rendering on any machine: the
// arrow keys turn the simulated head, W and S move it. Upstream reports "not available".
//
#include <cmath>
#include <cstdlib>
#include <memory>

#include <spdlog/spdlog.h>

#include "exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "framework/xr/openxr/openXrBackend.h"
#include "framework/xr/simulatedXrBackend.h"
#include "framework/xr/xrManager.h"
#include "platform/input/keyboard.h"

using namespace visutwin::canvas;

class VrBasicExample final : public ExampleApp
{
public:
    VrBasicExample() : ExampleApp({}) {}

protected:
    void configureDevice(GraphicsDeviceOptions& options) override
    {
        // The runtime, where there is one, has a say in the device a session will share.
        _openXr = createOpenXrBackend();
        if (_openXr) {
            _openXr->configureGraphicsDevice(options);
        }
    }

    void configure(AppOptions& options) override
    {
        _xr = std::make_shared<XrManager>();
        options.xr = _xr;
    }

    bool create() override
    {
        _envAtlasAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _galleryAsset = std::make_unique<Asset>("gallery", AssetType::CONTAINER, assetPath("models/xr_gallery.glb"));

        if (Texture* envAtlas = _envAtlasAsset->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setSkyboxMip(0);
        scene()->setExposure(0.5f);

        auto* container = _galleryAsset->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("VR Basic needs models/xr_gallery.glb");
            return false;
        }
        root()->addChild(container->instantiateRenderEntity());

        // The session's reference space is the camera's parent: the rig stands on the floor.
        _rig = new Entity();
        _rig->setEngine(engine());
        _rig->setName("rig");
        root()->addChild(_rig);

        Entity* camera = createCamera(Vector3(1.35f, 2.45f, 3.0f));
        camera->lookAt(Vector3(0.0f, 1.25f, 0.0f));
        _rig->addChild(camera);
        _camera = camera->findComponent<CameraComponent>();
        if (!_camera || !_camera->camera()) {
            return false;
        }
        _camera->camera()->setClearColor(Color(44.0f / 255.0f, 62.0f / 255.0f, 80.0f / 255.0f, 1.0f));
        _camera->camera()->setFarClip(10000.0f);

        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            component->setType(LightType::LIGHTTYPE_SPOT);
            component->setRange(30.0f);
        }
        light->setLocalPosition(0.0f, 10.0f, 0.0f);
        root()->addChild(light);

        // A headset where there is one, the simulated one otherwise.
        if (_openXr && _openXr->isAvailable(XrSessionType::Vr)) {
            _xr->setBackend(_openXr);
            spdlog::info("Immersive VR is available through OpenXR");
        } else {
            SimulatedXrOptions simulated;
            simulated.viewWidth = 900;
            simulated.viewHeight = 700;
            // The window mirrors one eye: give the eye the window's shape.
            const float halfVertical = 0.6f;
            const float halfHorizontal = std::atan(std::tan(halfVertical) * 900.0f / 700.0f);
            simulated.fov = {-halfHorizontal, halfHorizontal, halfVertical, -halfVertical};
            _simulated = std::make_shared<SimulatedXrBackend>(simulated);
            _xr->setBackend(_simulated);
            spdlog::info("No OpenXR headset: immersive VR runs on the simulated headset "
                "(arrow keys turn the head, W and S move it)");
        }
        _xr->on("start", [] { spdlog::info("Immersive VR session has started"); });
        _xr->on("end", [] { spdlog::info("Immersive VR session has ended"); });
        spdlog::info("Keys: X starts and ends the session");
        // A capture drops the keyboard: VISUTWIN_XR_START=1 starts the session with the example.
        if (const char* start = std::getenv("VISUTWIN_XR_START"); start && start[0] == '1') {
            toggleSession();
        }
        return true;
    }

    void update(const float dt) override
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        if (keyboard->wasPressed(Key::X)) {
            toggleSession();
        }
        if (_simulated && _xr->active()) {
            const float turn = 60.0f * dt;
            _yaw += (keyboard->isPressed(Key::Left) ? turn : 0.0f) - (keyboard->isPressed(Key::Right) ? turn : 0.0f);
            _pitch += (keyboard->isPressed(Key::Up) ? turn : 0.0f) - (keyboard->isPressed(Key::Down) ? turn : 0.0f);
            const Quaternion rotation = Quaternion::fromEulerAngles(_pitch, _yaw, 0.0f);
            const float step = (keyboard->isPressed(Key::W) ? 1.5f : 0.0f) - (keyboard->isPressed(Key::S) ? 1.5f : 0.0f);
            _position = _position + rotation * Vector3(0.0f, 0.0f, -step * dt);
            _simulated->setHeadPose({_position, rotation});
        }
    }

    void toggleSession()
    {
        if (_xr->active()) {
            _xr->end();
            return;
        }
        XrSessionOptions options;
        options.type = XrSessionType::Vr;
        options.spaceType = XrSpaceType::LocalFloor;
        _xr->start(_camera, options, [](const std::string* error) {
            if (error) {
                spdlog::warn("Immersive VR failed to start: {}", *error);
            }
        });
    }

    void destroy() override
    {
        _xr->end();
        _xr->setBackend(nullptr);
        _simulated.reset();
        _openXr.reset();
    }

private:
    std::shared_ptr<XrManager> _xr;
    std::shared_ptr<SimulatedXrBackend> _simulated;
    std::shared_ptr<XrBackend> _openXr;
    std::unique_ptr<Asset> _envAtlasAsset;
    std::unique_ptr<Asset> _galleryAsset;
    Entity* _rig = nullptr;
    CameraComponent* _camera = nullptr;
    Vector3 _position{0.0f, 1.6f, 3.0f};
    float _yaw = 0.0f;
    float _pitch = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(VrBasicExample)
