// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/mesh-morph-many.
//
// The Khronos MorphStressTest model (one node, one mesh of two primitives, eight morph
// targets each) at the origin under the helipad environment atlas (skybox mip 2, exposure
// 1.2). A shadow-casting directional light aimed at (45, 45, 0) degrees (shadow bias 0.5,
// normal offset bias 0.2, shadow distance 25) lights it, and the camera sits at (0, 4, 9)
// looking at the origin. Every frame the eight weights of the SECOND primitive's morph
// instance follow |sin(time + i * 0.4)|, so all eight targets are active at once with
// staggered phases; the first primitive keeps its file weights, as upstream leaves it.
//
// Model: MorphStressTest by Ed Mackey, CC BY 4.0.
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/meshInstance.h"
#include "scene/morph.h"
#include "scene/morphInstance.h"

using namespace visutwin::canvas;

class MeshMorphManyExample final: public ExampleApp
{
public:
    MeshMorphManyExample(): ExampleApp({.title = "Mesh Morph Many"}) {}

protected:
    bool create() override
    {
        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas", AssetType::TEXTURE, assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _morph = std::make_unique<Asset>(
            "glb", AssetType::CONTAINER, assetPath("models/morph-stress-test.glb"));

        Texture* helipad = _helipad->resourceAs<Texture>();
        auto* container = _morph->resourceAs<ContainerResource>();
        if (!helipad || !container) {
            spdlog::error("Failed to load the helipad environment atlas or morph-stress-test.glb");
            return false;
        }

        // The sky dome.
        scene()->setSkyboxMip(2);
        scene()->setExposure(1.2f);
        scene()->setEnvAtlas(helipad);

        // An instance of the morph target model.
        Entity* morphEntity = container->instantiateRenderEntity();
        if (!morphEntity) {
            spdlog::error("morph-stress-test.glb has no renderable content");
            return false;
        }
        root()->addChild(morphEntity);

        // The morph instance the weights are applied to: the second primitive's.
        if (auto* render = morphEntity->findComponent<RenderComponent>()) {
            const auto& meshInstances = render->meshInstances();
            if (meshInstances.size() > 1 && meshInstances[1]) {
                _morphInstance = meshInstances[1]->morphInstance();
            }
        }
        if (!_morphInstance) {
            spdlog::error("morph-stress-test.glb: the second mesh instance carries no morph instance");
            return false;
        }

        // A shadow-casting directional light.
        auto* lightEntity = createDirectionalLight(Vector3(45.0f, 45.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* light = lightEntity->findComponent<LightComponent>()) {
            light->setShadowBias(0.5f);
            light->setShadowNormalBias(0.2f);
            light->setShadowDistance(25.0f);
        }

        // The camera.
        auto* camera = createCamera(Vector3(0.0f, 4.0f, 9.0f));
        camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        spdlog::info("Eight morph targets driven along staggered sine curves");
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Modify the weights of all morph targets along a sine curve.
        const int targetsCount = _morphInstance->morph()->targetCount();
        for (int i = 0; i < targetsCount; ++i) {
            _morphInstance->setWeight(i, std::abs(std::sin(_time + static_cast<float>(i) * 0.4f)));
        }
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _morph;
    MorphInstance* _morphInstance = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MeshMorphManyExample)
