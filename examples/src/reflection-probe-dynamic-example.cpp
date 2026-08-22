// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Dynamic reflection-probe demo: a chrome sphere reflects a ring of orbiting
// emissive boxes via a runtime scene-capture cubemap probe. Each frame the
// ReflectionProbe renders the scene into six cube faces from the sphere centre
// and installs it as the scene reflection probe, so the reflection tracks the
// moving boxes live (unlike a static, hand-authored cubemap). The sphere sits
// on its own layer excluded from the probe capture, so it does not reflect
// itself. Esc quits.
//
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/extras/reflectionProbe.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

constexpr int LAYERID_REFLECTIVE = 50;
constexpr int BOX_COUNT = 5;

class ReflectionProbeDynamicExample final: public ExampleApp
{
public:
    ReflectionProbeDynamicExample(): ExampleApp({.title = "Dynamic Reflection Probe"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.05f, 0.05f, 0.06f);

        // Helipad environment atlas + skybox backdrop (matches the upstream
        // reflection-cubemap example: setSkyboxMip(0), setSkyboxIntensity(2.0)).
        // Installed BEFORE the render loop so the skybox is present when the dynamic
        // probe captures its cube faces — the chrome sphere then reflects BOTH the
        // captured orbiting boxes AND the environment.
        scene()->setSkyboxMip(0);
        scene()->setSkyboxIntensity(2.0f);

        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );

        if (const auto helipadResource = _helipad->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));
        } else {
            spdlog::error("Failed to load helipad env atlas texture");
        }

        // Layer composition: WORLD (orbiting boxes + floor), a dedicated REFLECTIVE
        // layer for the chrome sphere (so the probe does not capture it), SKYBOX.
        const auto defaultComposition = scene()->layers();
        const auto worldLayer = defaultComposition->getLayerById(LAYERID_WORLD);
        const auto skyboxLayer = defaultComposition->getLayerById(LAYERID_SKYBOX);
        auto reflectiveLayer = std::make_shared<Layer>("Reflective", LAYERID_REFLECTIVE);
        auto composition = std::make_shared<LayerComposition>("reflection-probe-composition");
        composition->pushOpaque(worldLayer);
        composition->pushOpaque(reflectiveLayer);
        if (skyboxLayer) {
            composition->pushOpaque(skyboxLayer);
        }
        scene()->setLayers(composition);

        // Chrome sphere on the REFLECTIVE layer (excluded from probe capture).
        _chrome = std::make_shared<StandardMaterial>();
        _chrome->setDiffuse(Color(0.95f, 0.95f, 0.95f, 1.0f));
        _chrome->setMetalness(1.0f);
        _chrome->setGlossInvert(true);
        _chrome->setGloss(0.02f);
        root()->addChild(createEntity(_chrome.get(), "sphere",
            Vector3(0.0f, 1.2f, 0.0f), Vector3(2.4f, 2.4f, 2.4f), {LAYERID_REFLECTIVE}));

        // A muted floor (WORLD) so the probe/sphere have a ground below them.
        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.10f, 0.11f, 0.13f, 1.0f));
        _floorMaterial->setMetalness(0.0f);
        _floorMaterial->setGlossInvert(true);
        _floorMaterial->setGloss(0.6f);
        root()->addChild(createEntity(_floorMaterial.get(), "plane",
            Vector3(0.0f, 0.0f, 0.0f), Vector3(30.0f, 1.0f, 30.0f), {LAYERID_WORLD}));

        // Ring of emissive colored boxes (WORLD) — the content the sphere reflects.
        const Color boxColors[BOX_COUNT] = {
            Color(1.0f, 0.15f, 0.12f, 1.0f), Color(0.15f, 1.0f, 0.25f, 1.0f),
            Color(0.2f, 0.4f, 1.0f, 1.0f), Color(1.0f, 0.85f, 0.15f, 1.0f),
            Color(0.9f, 0.2f, 1.0f, 1.0f)};
        for (int i = 0; i < BOX_COUNT; ++i) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.02f, 0.02f, 0.02f, 1.0f));
            material->setEmissive(boxColors[i]);
            material->setEmissiveIntensity(3.0f);
            _boxMaterials.push_back(material);

            auto* box = createEntity(material.get(), "box",
                Vector3(0.0f, 1.2f, 0.0f), Vector3(0.9f, 0.9f, 0.9f), {LAYERID_WORLD});
            root()->addChild(box);
            _boxes.push_back(box);
        }

        // Dynamic reflection probe — constructed BEFORE the main camera so its six
        // face cameras render first each frame. Captures WORLD + SKYBOX (not the
        // sphere's REFLECTIVE layer).
        _probe = std::make_unique<ReflectionProbe>(engine(), 128);
        _probe->setPosition(Vector3(0.0f, 1.2f, 0.0f));
        _probe->setBox(Vector3(-8.0f, 0.0f, -8.0f), Vector3(8.0f, 8.0f, 8.0f), true);
        _probe->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        _probe->setNearFar(0.1f, 100.0f);
        _probe->setDynamic(true);

        // Main camera (presentation) — renders all three layers.
        _camera = createCamera(Vector3(0.0f, 2.6f, 8.5f), Vector3(-10.0f, 0.0f, 0.0f));
        _camera->findComponent<CameraComponent>()
            ->setLayers({LAYERID_WORLD, LAYERID_REFLECTIVE, LAYERID_SKYBOX});

        createDirectionalLight(Vector3(55.0f, -30.0f, 0.0f), Color(1.0f, 0.98f, 0.95f, 1.0f));

        spdlog::info("Dynamic reflection probe: chrome sphere reflects orbiting emissive boxes (live capture). Esc quits.");

        return true;
    }

    void update(float) override
    {
        const float elapsed = elapsedTime();

        // Orbit the emissive boxes around the sphere so the reflection moves.
        for (int i = 0; i < BOX_COUNT; ++i) {
            const float theta = elapsed * 0.7f + static_cast<float>(i) * (2.0f * 3.14159265f / BOX_COUNT);
            const float radius = 4.2f;
            _boxes[i]->setLocalPosition(std::cos(theta) * radius, 1.2f + std::sin(theta * 1.3f) * 0.9f,
                std::sin(theta) * radius);
        }

        // Slow camera orbit so the parallax reads as 3D.
        const float camAngle = std::sin(elapsed * 0.2f) * 0.6f;
        _camera->setLocalPosition(std::sin(camAngle) * 8.7f, 2.6f, std::cos(camAngle) * 8.7f);
        _camera->setLocalEulerAngles(-10.0f, camAngle * 57.2958f, 0.0f);
    }

    void postRender() override
    {
        _probe->update();   // regenerate cube mips + keep the probe installed
    }

    void destroy() override
    {
        // The probe owns cameras and render targets borrowed from the engine, so
        // it has to go while the engine is still alive.
        _probe.reset();
    }

private:
    Entity* createEntity(Material* material, const char* type, const Vector3& position,
        const Vector3& scale, const std::vector<int>& layers) const
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
        entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
        if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
            render->setMaterial(material);
            render->setType(type);
            render->setLayers(layers);
        }
        return entity;
    }

    std::unique_ptr<Asset> _helipad;
    std::shared_ptr<StandardMaterial> _chrome;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _boxMaterials;
    std::vector<Entity*> _boxes;
    std::unique_ptr<ReflectionProbe> _probe;
    Entity* _camera = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionProbeDynamicExample)
