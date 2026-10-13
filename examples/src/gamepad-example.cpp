// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream input/gamepad.
//
// The statue at the origin under the helipad environment atlas (exposure 1.6, skybox mip 1,
// no light), seen from (0, 7, 25) by a camera clearing to (0.4, 0.45, 0.5) with ACES tone
// mapping. The first pad's D-pad turns the statue in world space: holding left or right
// turns it about Y by one degree a frame, and each press of up or down tilts it about X by
// one degree.
//
// D-pad left / right (held)  rotate the statue about Y.
// D-pad up / down (pressed)  tilt the statue about X.
//
// DEVIATION: upstream creates a GamePads of its own and calls its update() at the top of
// every frame; here the statue reads the engine's GamePads, which the engine updates once a
// frame itself, so this example does not call update(). The pad is addressed by SDL's
// layout (PadButton::DPadLeft ...) rather than upstream's standard-mapping indices.
//
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/input/gamePads.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

namespace
{
    // The first connected pad
    constexpr size_t kPad1 = 0;
}

class GamepadExample final: public ExampleApp
{
public:
    GamepadExample(): ExampleApp({.title = "Gamepad"}) {}

protected:
    bool create() override
    {
        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _statue = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/statue.glb"));

        Texture* helipadTexture = _helipad->resourceAs<Texture>();
        ContainerResource* statueContainer = _statue->resourceAs<ContainerResource>();
        if (!helipadTexture || !statueContainer) {
            spdlog::error("Failed to load the helipad environment atlas or the statue");
            return false;
        }

        // The skybox
        scene()->setEnvAtlas(helipadTexture);
        scene()->setExposure(1.6f);
        scene()->setSkyboxMip(1);

        // A camera
        auto* camera = createCamera(Vector3(0.0f, 7.0f, 25.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
            component->setToneMapping(TONEMAP_ACES);
        }

        _entity = statueContainer->instantiateRenderEntity();
        root()->addChild(_entity);

        spdlog::info("D-pad left / right rotates the statue, up / down tilts it");
        return true;
    }

    void update(float) override
    {
        const auto* gamepads = engine()->gamepads();
        if (!gamepads || !_entity) {
            return;
        }
        if (gamepads->isPressed(kPad1, PadButton::DPadLeft)) {
            _entity->rotate(0.0f, -1.0f, 0.0f);
        }
        if (gamepads->isPressed(kPad1, PadButton::DPadRight)) {
            _entity->rotate(0.0f, 1.0f, 0.0f);
        }
        if (gamepads->wasPressed(kPad1, PadButton::DPadUp)) {
            _entity->rotate(-1.0f, 0.0f, 0.0f);
        }
        if (gamepads->wasPressed(kPad1, PadButton::DPadDown)) {
            _entity->rotate(1.0f, 0.0f, 0.0f);
        }
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;
    Entity* _entity = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(GamepadExample)
