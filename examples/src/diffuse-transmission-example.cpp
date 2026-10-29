// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.10.2026
//
// Port of upstream materials/diffuse-transmission.
//
// Khronos' DiffuseTransmissionTest (Eric Chadwick / Darmstadt Graphics Group GmbH, CC BY 4.0,
// assets/models/DiffuseTransmissionTest.txt). Diffuse transmission lets light pass through a
// thin surface, a leaf, a sheet of paper or a lampshade, which then glows when lit from behind.
// The model's own directional light shines towards the camera, so each square is seen from the
// side facing away from the light. The columns raise the transmitted fraction from 0 to 1; the
// first row transmits white light, the second tints it red, the third masks it with the stripes
// of the fraction map and the fourth colours it with the colour map. The table-mountain
// environment lights the side facing the camera; neutral tone mapping.
//
#include <memory>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class DiffuseTransmissionExample final : public ExampleApp
{
public:
    DiffuseTransmissionExample() : ExampleApp({.title = "Diffuse Transmission"}) {}

protected:
    bool create() override
    {
        _envAtlasAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER,
            assetPath("models/DiffuseTransmissionTest.glb"));

        // The environment lights the side of the squares facing the camera, showing their base colour.
        if (Texture* envAtlas = _envAtlasAsset->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setSkyboxMip(1);

        auto* container = _modelAsset->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Diffuse Transmission needs models/DiffuseTransmissionTest.glb");
            return false;
        }
        // The lights of a glb are imported disabled: enable the model's directional light, which
        // shines towards the camera.
        Entity* model = container->instantiateRenderEntity();
        for (auto* light : model->findComponents<LightComponent>()) {
            light->setEnabled(true);
        }
        root()->addChild(model);

        // Looking at the model straight from the front.
        Entity* camera = createCamera(Vector3(1.18f, -1.57f, 9.47f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        if (!cameraComponent || !cameraComponent->camera()) {
            return false;
        }
        cameraComponent->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        cameraComponent->setToneMapping(TONEMAP_NEUTRAL);
        if (auto* controls = addOrbitControls(camera, Vector3(1.18f, -1.57f, -0.25f))) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->storeResetState();
        }
        return true;
    }

private:
    std::unique_ptr<Asset> _envAtlasAsset;
    std::unique_ptr<Asset> _modelAsset;
};

VISUTWIN_EXAMPLE_MAIN(DiffuseTransmissionExample)
