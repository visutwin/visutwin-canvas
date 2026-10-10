// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream test/two-sided-lighting.
//
// Two copies of TwoSidedPlane.glb, a textured plane whose glTF material is double-sided,
// side by side under the morning env atlas (skybox mip 1, intensity 0.4) and an orange
// directional light at (45, 30, 0), intensity 2. The left copy keeps the material's
// normal map, applied through the tangent frame; the right copy uses a clone of the
// material with the normal map removed, so it is shaded with the interpolated vertex
// normal. A white omni light (intensity 4, range 10, no shadows), marked by a small
// emissive sphere, orbits the origin in the XY plane at radius 2 and so passes in front
// of and behind both planes: each plane's back face must be lit from the viewer's side
// exactly as its front face is, with and without the normal map. The camera sits at
// (0, 2, 6), orbiting the origin with ACES tone mapping.
//
// Upstream's example has no control panel, so there are no keys beyond the orbit camera:
//   R reset camera | F1 HUD | Esc quit | LMB/RMB orbit, Shift/MMB pan, Wheel zoom
//
// DEVIATION: the camera controls orbit only (no fly mode), as in every example here,
// where upstream's leave fly mode on.
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr const char* kModel = "models/TwoSidedPlane.glb";

    // Radius of the omni light's orbit round the origin.
    constexpr float kOrbitRadius = 2.0f;
}

class TwoSidedLightingExample final: public ExampleApp
{
public:
    TwoSidedLightingExample(): ExampleApp({.title = "Two-Sided Lighting"}) {}

protected:
    bool create() override
    {
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

        _model = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath(kModel));
        ContainerResource* container = _model->resourceAs<ContainerResource>();
        if (!container) {
            spdlog::error("Failed to load {}", kModel);
            return false;
        }

        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(0.4f);
        scene()->setEnvAtlas(morningTexture);

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 0.8f, 0.25f, 1.0f), 2.0f, false);

        // The plane with its normal map, which is applied through the tangent frame.
        Entity* entity = container->instantiateRenderEntity();
        if (!entity) {
            spdlog::error("Failed to instantiate {}", kModel);
            return false;
        }
        entity->setEngine(engine());
        entity->setLocalPosition(-1.1f, 0.0f, 0.0f);
        root()->addChild(entity);

        // The same plane without its normal map, which builds no tangent frame and so is
        // shaded with the vertex normal. The container's materials are shared by every
        // instance, so the change goes on a clone.
        Entity* plain = container->instantiateRenderEntity();
        if (!plain) {
            spdlog::error("Failed to instantiate {}", kModel);
            return false;
        }
        plain->setEngine(engine());
        MeshInstance* plainInstance = firstMeshInstance(plain);
        auto* sourceMaterial = plainInstance ? dynamic_cast<StandardMaterial*>(plainInstance->material()) : nullptr;
        if (!sourceMaterial) {
            spdlog::error("{} has no standard material to clone", kModel);
            return false;
        }
        auto plainMaterial = std::static_pointer_cast<StandardMaterial>(sourceMaterial->clone());
        plainMaterial->setNormalMap(nullptr);
        plainInstance->setMaterial(plainMaterial);
        plain->setLocalPosition(1.1f, 0.0f, 0.0f);
        root()->addChild(plain);

        auto* camera = createCamera(Vector3(0.0f, 2.0f, 6.0f));
        if (auto* cameraComp = camera->findComponent<CameraComponent>()) {
            cameraComp->setToneMapping(TONEMAP_ACES);
        }
        addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f));

        // Emissive white marker, unaffected by the lights.
        _lightMaterial = std::make_shared<StandardMaterial>();
        _lightMaterial->setName("light-marker");
        _lightMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _lightMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _lightMaterial->setUseLighting(false);

        _omniLight = new Entity();
        _omniLight->setName("omni light");
        _omniLight->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(_omniLight->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_OMNI);
            light->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            light->setIntensity(4.0f);
            light->setRange(10.0f);
            light->setCastShadows(false);
        }
        if (auto* render = static_cast<RenderComponent*>(_omniLight->addComponent<RenderComponent>())) {
            render->setType("sphere");
            render->setMaterial(_lightMaterial.get());
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }
        _omniLight->setLocalScale(0.1f, 0.1f, 0.1f);
        root()->addChild(_omniLight);

        spdlog::info("Two-sided lighting: left plane normal-mapped, right plane vertex normals; "
                     "the omni light orbits both sides of them.");
        spdlog::info("Orbit: LMB/RMB orbit, Wheel zoom, R reset, Esc quit.");

        return true;
    }

    void update(const float dt) override
    {
        _time += dt * 0.5f;
        if (_omniLight) {
            _omniLight->setPosition(std::cos(_time) * kOrbitRadius, std::sin(_time) * kOrbitRadius, 0.0f);
        }
    }

private:
    static MeshInstance* firstMeshInstance(Entity* entity)
    {
        for (auto* render : entity->findComponents<RenderComponent>()) {
            if (render && !render->meshInstances().empty()) {
                return render->meshInstances()[0];
            }
        }
        return nullptr;
    }

    std::unique_ptr<Asset> _morning;
    std::unique_ptr<Asset> _model;
    std::shared_ptr<StandardMaterial> _lightMaterial;
    Entity* _omniLight = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TwoSidedLightingExample)
