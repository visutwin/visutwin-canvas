// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream graphics-advanced/portal.
//
// A portal made with the stencil buffer. A plane filling the inside of a portal frame renders
// first, in a layer of its own ahead of the world layer, writing no colour and no depth: every
// fragment of it that passes the depth test increments the stencil from 0 to 1. A statue then draws
// only where the stencil is not 0 (inside the portal) and the bitmoji character only where it is 0
// (outside), both standing at the same spot. The portal frame model, the plane, the statue and the
// character turn together, swinging 40 degrees either way (sin(t)). A white directional light turned
// (45, 35, 0); the helipad environment (skybox mip 1, intensity 0.7); the camera at (7, 5.5, 7.1)
// turned (-27, 45, 0) renders the world, portal, skybox and UI layers and tone-maps ACES.
//
// Keys standing in for upstream's controls panel:
//   C  render through a camera frame (4x MSAA, its scene target with stencil), off to begin with
//
// DEVIATION: the stencil test and operations are set on the mesh instances
// (MeshInstance::setStencil), where upstream sets them on the materials.
//
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/stencilParameters.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // The id of the layer the portal plane renders into, outside the built-in range
    constexpr int LAYERID_PORTAL = 20;

    // Sets the same stencil parameters on every mesh instance under `entity`
    void setStencil(Entity* entity, const std::shared_ptr<StencilParameters>& stencil)
    {
        for (auto* render : entity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                meshInstance->setStencil(stencil, stencil);
            }
        }
    }

    Entity* instantiate(Asset& asset, Engine* engine)
    {
        auto* container = asset.resourceAs<ContainerResource>();
        Entity* entity = container ? container->instantiateRenderEntity() : nullptr;
        if (entity) {
            entity->setEngine(engine);
        }
        return entity;
    }
}

class PortalExample final : public ExampleApp
{
public:
    PortalExample() : ExampleApp({.title = "Portal"}) {}

protected:
    bool create() override
    {
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _portalAsset = std::make_unique<Asset>("portal", AssetType::CONTAINER, assetPath("models/portal.glb"));
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _bitmojiAsset = std::make_unique<Asset>("bitmoji", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        Texture* helipad = _helipadAsset->resourceAs<Texture>();
        Entity* portalEntity = instantiate(*_portalAsset, engine());
        Entity* statue = instantiate(*_statueAsset, engine());
        Entity* bitmoji = instantiate(*_bitmojiAsset, engine());
        if (!helipad || !portalEntity || !statue || !bitmoji) {
            spdlog::error("portal needs the helipad atlas and the portal, statue and bitmoji models");
            return false;
        }

        // Set skybox
        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(0.7f);

        // The portal layer, where the portal plane is written to the stencil buffer: it has to
        // render first, so it goes in before the world layer
        _portalLayer = std::make_shared<Layer>("Portal", LAYERID_PORTAL);
        scene()->layers()->insert(_portalLayer, 0);

        // The camera renders both the world and the portal layers
        _camera = createCamera(Vector3(7.0f, 5.5f, 7.1f));
        _camera->setLocalEulerAngles(-27.0f, 45.0f, 0.0f);
        auto* cameraComponent = _camera->findComponent<CameraComponent>();
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_PORTAL, LAYERID_SKYBOX, LAYERID_IMMEDIATE, LAYERID_UI});
        cameraComponent->setToneMapping(TONEMAP_ACES);

        // A directional light
        createDirectionalLight(Vector3(45.0f, 35.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, false);

        // The root of the scene, which turns
        _group = new Entity();
        _group->setEngine(engine());
        root()->addChild(_group);

        // The portal plane, filling the inside of the portal frame. It is written to the stencil
        // buffer only (incremented from 0 to 1), with no colour or depth.
        _portalMaterial = std::make_shared<StandardMaterial>();
        auto blend = std::make_shared<BlendState>();
        blend->setRedWrite(false);
        blend->setGreenWrite(false);
        blend->setBlueWrite(false);
        blend->setAlphaWrite(false);
        _portalMaterial->setBlendState(blend);
        auto depth = std::make_shared<DepthState>();
        depth->setDepthWrite(false);
        _portalMaterial->setDepthState(depth);
        auto* portal = new Entity();
        portal->setName("Portal");
        portal->setEngine(engine());
        auto* portalRender = static_cast<RenderComponent*>(portal->addComponent<RenderComponent>());
        portalRender->setLayers({LAYERID_PORTAL});
        portalRender->setMaterial(_portalMaterial.get());
        portalRender->setType("plane");
        portal->setLocalPosition(0.0f, 0.4f, -0.3f);
        portal->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        portal->setLocalScale(3.7f, 1.0f, 6.7f);
        _group->addChild(portal);
        auto increment = std::make_shared<StencilParameters>();
        increment->setPassOperation(StencilOperation::IncrementClamp);
        setStencil(portal, increment);

        // The portal frame
        portalEntity->setLocalPosition(0.0f, -3.0f, 0.0f);
        portalEntity->setLocalScale(0.02f, 0.02f, 0.02f);
        _group->addChild(portalEntity);

        // The statue, visible inside the portal only: drawn where the stencil is not 0
        statue->setLocalPosition(0.0f, -1.0f, -2.0f);
        statue->setLocalScale(0.25f, 0.25f, 0.25f);
        _group->addChild(statue);
        auto inside = std::make_shared<StencilParameters>();
        inside->setCompareFunction(StencilCompareFunction::NotEqual);
        inside->setReference(0);
        setStencil(statue, inside);

        // The bitmoji, visible outside the portal only: drawn where the stencil is 0
        bitmoji->setLocalPosition(0.0f, -1.0f, -2.0f);
        bitmoji->setLocalScale(2.5f, 2.5f, 2.5f);
        _group->addChild(bitmoji);
        auto outside = std::make_shared<StencilParameters>();
        outside->setCompareFunction(StencilCompareFunction::Equal);
        outside->setReference(0);
        setStencil(bitmoji, outside);

        spdlog::info("Keys: C toggles rendering through a camera frame (4x MSAA with stencil)");
        return true;
    }

    void update(const float dt) override
    {
        // Rotate the scene
        _time += dt;
        _group->setLocalEulerAngles(0.0f, std::sin(_time) * 40.0f, 0.0f);

        const auto* keyboard = engine()->keyboard();
        if (keyboard && keyboard->wasPressed(Key::C)) {
            auto* cameraComponent = _camera->findComponent<CameraComponent>();
            auto rendering = cameraComponent->rendering();
            rendering.useCameraFrame = !rendering.useCameraFrame;
            rendering.stencil = rendering.useCameraFrame;
            rendering.samples = rendering.useCameraFrame ? 4 : 1;
            cameraComponent->setRendering(rendering);
            spdlog::info("camera frame: {}", rendering.useCameraFrame ? "on" : "off");
        }
    }

private:
    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<Asset> _portalAsset;
    std::unique_ptr<Asset> _statueAsset;
    std::unique_ptr<Asset> _bitmojiAsset;
    std::shared_ptr<Layer> _portalLayer;
    std::shared_ptr<StandardMaterial> _portalMaterial;
    Entity* _camera = nullptr;
    Entity* _group = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(PortalExample)
