// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream input/mouse.
//
// The statue at the origin under the helipad environment atlas (exposure 1.6, skybox mip 1,
// no light), seen from (0, 7, 25) by a camera clearing to (0.4, 0.45, 0.5) with ACES tone
// mapping. Dragging with the left mouse button turns the statue about Y: every mouse move
// while the button is held adds its horizontal movement to an accumulator, and the statue's
// local euler angles are set to (0.2 y, 0.2 x, 0) with y held at 0.
//
// Drag with the left mouse button  rotate the statue.
//
// DEVIATION: upstream creates a Mouse of its own on the page and detaches it on destroy;
// here the handler listens to the engine's mouse, which the example host feeds, and is
// removed on destroy while the device stays attached. Upstream's event carries the button
// states; here the handler asks the device whether the left button is held.
//
#include <memory>

#include "../exampleApp.h"
#include "core/eventHandler.h"
#include "framework/assets/asset.h"
#include "platform/input/inputConstants.h"
#include "platform/input/mouse.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class MouseExample final: public ExampleApp
{
public:
    MouseExample(): ExampleApp({.title = "Mouse"}) {}

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

        Mouse* mouse = engine()->mouse();
        if (!mouse) {
            spdlog::error("The engine has no mouse");
            return false;
        }
        _mouseMove = mouse->on("mousemove", [this, mouse](const MouseEvent& event) {
            if (mouse->isPressed(MouseButton::Left)) {
                _x += event.dx;
                _entity->setLocalEulerAngles(0.2f * _y, 0.2f * _x, 0.0f);
            }
        });

        spdlog::info("Drag with the left mouse button to rotate the statue");
        return true;
    }

    void destroy() override
    {
        if (_mouseMove) {
            _mouseMove->off();
            _mouseMove.reset();
        }
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;
    Entity* _entity = nullptr;

    EventHandlePtr _mouseMove;
    float _x = 0.0f;
    const float _y = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MouseExample)
