// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream input/keyboard.
//
// The statue at the origin under the helipad environment atlas (exposure 1.6, skybox mip 1,
// no light), seen from (0, 7, 25) by a camera clearing to (0.4, 0.45, 0.5) with ACES tone
// mapping. Holding the left or right arrow key turns the statue about the world Y axis by
// one degree a frame.
//
// Left / Right arrow  rotate the statue.
//
// DEVIATION: upstream creates a Keyboard of its own on the page; here the statue reads the
// engine's keyboard, which the example host feeds.
//
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class KeyboardExample final: public ExampleApp
{
public:
    KeyboardExample(): ExampleApp({.title = "Keyboard"}) {}

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

        spdlog::info("Hold the left or right arrow key to rotate the statue");
        return true;
    }

    void update(float) override
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard || !_entity) {
            return;
        }
        if (keyboard->isPressed(Key::Left)) {
            _entity->rotate(0.0f, -1.0f, 0.0f);
        }
        if (keyboard->isPressed(Key::Right)) {
            _entity->rotate(0.0f, 1.0f, 0.0f);
        }
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;
    Entity* _entity = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(KeyboardExample)
