// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// GLB loading reference: loads antique_camera.glb, reports what the parser
// actually produced (render components, mesh instances), auto-frames the model
// from its bounds so an asset of any scale stays visible, and spins it.
//
#include <algorithm>
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "core/shape/boundingBox.h"
#include "framework/assets/asset.h"

using namespace visutwin::canvas;

class GlbLoaderExample final: public ExampleApp
{
public:
    GlbLoaderExample(): ExampleApp({.title = "GLB Loader Reference"}) {}

protected:
    bool create() override
    {
        // Camera setup
        auto* cameraEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        auto* camera = cameraEntity->findComponent<CameraComponent>();
        if (!camera || !camera->camera()) {
            spdlog::error("Failed to create camera component");
            return false;
        }
        camera->camera()->setClearColor(Color(0.08f, 0.1f, 0.14f, 1.0f));
        camera->camera()->setFov(55.0f);

        // Light setup
        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.6f);

        _glbAsset = std::make_unique<Asset>(
            "statue",
            AssetType::CONTAINER,
            assetPath("models/antique_camera.glb")
        );

        const auto resource = _glbAsset->resource();
        if (!resource) {
            spdlog::error("GLB load failed: asset resource is null");
            spdlog::error("Diagnostics: parser logs above include unsupported/invalid payload reasons.");
            return false;
        }
        if (!std::holds_alternative<ContainerResource*>(*resource)) {
            spdlog::error("GLB load failed: expected ContainerResource, got different asset payload type");
            return false;
        }

        auto* container = std::get<ContainerResource*>(*resource);
        if (!container) {
            spdlog::error("GLB load failed: container payload is null");
            return false;
        }

        _modelEntity = container->instantiateRenderEntity();
        if (!_modelEntity) {
            spdlog::error("GLB instantiate failed: instantiateRenderEntity returned null");
            return false;
        }
        _modelEntity->setEngine(engine());
        root()->addChild(_modelEntity);

        RenderableStats stats;
        gatherRenderableStats(_modelEntity, stats);
        spdlog::info(
            "GLB instantiate result: renderComponents={}, meshInstances={}",
            stats.renderComponents,
            stats.meshInstances
        );
        if (stats.meshInstances == 0) {
            spdlog::error("GLB diagnostics: model instantiated but produced no mesh instances.");
            spdlog::error("Likely unsupported payload for current parser path (extensions, accessor formats, or non-mesh content).");
            spdlog::error("See parser warnings above for unsupported textures/images/accessors.");
        }

        // Auto-frame the model so very large/small assets remain visible.
        const BoundingBox modelBounds = entityBounds(_modelEntity);
        const Vector3 center = modelBounds.center();
        const float radius = std::max(modelBounds.halfExtents().length(), 0.5f);
        const float distance = std::max(radius * 2.8f, 3.0f);
        cameraEntity->setLocalPosition(center + Vector3(0.0f, radius * 0.6f, distance));
        cameraEntity->lookAt(center);
        camera->camera()->setNearClip(std::max(0.01f, radius * 0.01f));
        camera->camera()->setFarClip(std::max(500.0f, radius * 40.0f));

        return true;
    }

    void update(float) override
    {
        _modelEntity->setLocalEulerAngles(0.0f, elapsedTime() * 18.0f, 0.0f);
    }

private:
    struct RenderableStats
    {
        int renderComponents = 0;
        int meshInstances = 0;
    };

    static void gatherRenderableStats(GraphNode* node, RenderableStats& stats)
    {
        if (!node) {
            return;
        }

        if (auto* entity = dynamic_cast<Entity*>(node)) {
            if (auto* render = entity->findComponent<RenderComponent>()) {
                stats.renderComponents++;
                stats.meshInstances += static_cast<int>(render->meshInstances().size());
            }
        }

        for (const auto& child : node->children()) {
            gatherRenderableStats(child.get(), stats);
        }
    }

    std::unique_ptr<Asset> _glbAsset;
    Entity* _modelEntity = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(GlbLoaderExample)
