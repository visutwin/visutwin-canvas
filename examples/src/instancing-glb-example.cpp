// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream graphics/instancing-glb.
//
// Loads simple-instancing.glb, whose nodes carry EXT_mesh_gpu_instancing: each mesh is
// drawn once per instance matrix in a single draw call, every matrix placing its
// instance in the node's space. All of the model's meshes cast shadows. A white
// directional light (intensity 2, euler (60, 30, 0)) casts a 2048 shadow out to 100
// units onto a grey 50 x 1 x 50 box ground at y = -2, and the table-mountain env atlas
// lights the scene and is shown as the skybox at mip 1. The camera (clear colour
// (0.2, 0.1, 0.1), far clip 100, ACES) starts at (15, 15, -25) and orbits the model's
// origin.
//
// LMB / RMB orbit, Shift / MMB pan, Wheel / Pinch zoom, R reset, Esc quit.
//
// DEVIATIONS:
// - upstream's orbitCamera script (inertia 0.2, distanceMax 60, frameOnStart off) is the
//   examples' CameraControls in orbit mode: the pivot is the model's bounds centre (the orbit camera's),
//   the distance is the authored camera position's, the zoom is capped at 60 and the
//   pitch at +/-90 degrees. Its damping is CameraControls' own, not the 0.2 s inertia.
// - upstream caps the pixel ratio at 2; the examples render at one pixel per point.
//
#include <memory>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr const char* kModel = "models/simple-instancing.glb";
}

class InstancingGlbExample final: public ExampleApp
{
public:
    InstancingGlbExample(): ExampleApp({.title = "Instancing GLB"}) {}

protected:
    bool create() override
    {
        _envAtlasAsset = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _glbAsset = std::make_unique<Asset>("glb", AssetType::CONTAINER, assetPath(kModel));

        Texture* envAtlas = _envAtlasAsset->resourceAs<Texture>();
        if (!envAtlas) {
            spdlog::error("Failed to load the table-mountain env atlas");
            return false;
        }
        ContainerResource* container = _glbAsset->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Failed to load {}", kModel);
            return false;
        }

        // Get the instance of the model set up with render components and add it to the scene
        Entity* entity = container->instantiateRenderEntity();
        if (!entity) {
            spdlog::error("Failed to instantiate {}", kModel);
            return false;
        }
        entity->setEngine(engine());
        for (auto* render : entity->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
        }
        root()->addChild(entity);

        // Create an Entity with a camera component
        auto* camera = createCamera(Vector3(15.0f, 15.0f, -25.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setClearColor(Color(0.2f, 0.1f, 0.1f, 1.0f));
            comp->camera()->setFarClip(100.0f);
            comp->setToneMapping(TONEMAP_ACES);
        }

        // Orbit the model with mouse and touch
        if (auto* controls = addOrbitControls(camera, entityBounds(entity).center())) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.0f, 60.0f));
            controls->storeResetState();
        }

        // Set skybox
        scene()->setEnvAtlas(envAtlas);
        scene()->setSkyboxMip(1);

        // Create an entity with a light component
        auto* light = createDirectionalLight(Vector3(60.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 2.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowDistance(100.0f);
            lightComp->setShadowNormalBias(0.05f);
            lightComp->setShadowResolution(2048);
        }

        // Create an Entity for the ground
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color::GRAY);
        createPrimitive("box", _groundMaterial.get(), Vector3(0.0f, -2.0f, 0.0f),
            Vector3(50.0f, 1.0f, 50.0f));

        spdlog::info("Instancing GLB: EXT_mesh_gpu_instancing meshes from {}. "
                     "Drag to orbit, wheel to zoom, R reset, Esc quit.", kModel);
        return true;
    }

private:
    std::unique_ptr<Asset> _envAtlasAsset;
    std::unique_ptr<Asset> _glbAsset;
    std::shared_ptr<StandardMaterial> _groundMaterial;
};

VISUTWIN_EXAMPLE_MAIN(InstancingGlbExample)
