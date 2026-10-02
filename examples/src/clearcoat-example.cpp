// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026
//
// Clearcoat material demo (parity with upstream materials/clear-coat): the
// Khronos ClearCoatTest.glb sample asset — six labelled columns of sphere/plane
// pairs comparing Base / Coating / Coated variants (partial coat masks, rough
// coat variations, base/coat/shared normal maps) — lit by the morning env atlas
// and a yellow directional light. The Coated column shows highlights from BOTH
// the base and coating layers.
//
// The GLB's materials author clearcoat via KHR_materials_clearcoat (factors +
// intensity/roughness/normal textures), parsed by glbParser::applyClearcoat.
//
#include <memory>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class ClearcoatExample final: public ExampleApp
{
public:
    ClearcoatExample(): ExampleApp({.title = "Clear Coat"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setSkyboxIntensity(1.5f);
        scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.0f, 70.0f, 0.0f));

        // Morning environment atlas.
        _morning = std::make_unique<Asset>(
            "morning-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/morning-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );

        Texture* morningTexture = _morning->resourceAs<Texture>();
        if (!morningTexture) {
            spdlog::error("Failed to load morning env atlas");
            return false;
        }
        scene()->setEnvAtlas(morningTexture);

        // Khronos ClearCoatTest sample model (KHR_materials_clearcoat), posed
        // at yaw 90, position (0,0,1), scale 0.8.
        _model = std::make_unique<Asset>(
            "clearcoat-test",
            AssetType::CONTAINER,
            assetPath("models/ClearCoatTest.glb")
        );

        ContainerResource* modelContainer = _model->resourceAs<ContainerResource>();
        if (!modelContainer) {
            spdlog::error("Failed to load ClearCoatTest.glb");
            return false;
        }
        auto* modelEntity = modelContainer->instantiateRenderEntity();
        modelEntity->setLocalEulerAngles(0.0f, 90.0f, 0.0f);
        modelEntity->setLocalPosition(0.0f, 0.0f, 1.0f);
        modelEntity->setLocalScale(0.8f, 0.8f, 0.8f);
        root()->addChild(modelEntity);

        // Yellow directional light, no shadows.
        createDirectionalLight(Vector3(45.0f, 180.0f, 0.0f), Color(1.0f, 1.0f, 0.0f, 1.0f), 1.0f, false);

        // Orbit camera: yaw 90, distance 12 around the model.
        auto* camera = createCamera(Vector3(12.0f, 0.0f, 1.0f), Vector3(0.0f, 90.0f, 0.0f));
        addOrbitControls(camera, Vector3(0.0f, 0.0f, 1.0f));

        spdlog::info("Clear coat: ClearCoatTest.glb (KHR_materials_clearcoat) — the Coated column "
                     "carries highlights from both Base and Coating layers.");
        spdlog::info("Orbit: LMB/RMB orbit, Wheel zoom, R reset, Esc quit.");

        return true;
    }

private:
    std::unique_ptr<Asset> _morning;
    std::unique_ptr<Asset> _model;
};

VISUTWIN_EXAMPLE_MAIN(ClearcoatExample)
