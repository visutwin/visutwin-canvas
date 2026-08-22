// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Dynamic batching example — port of upstream graphics/batching-dynamic.
//
// 500 procedural primitives orbit the origin in a spiral, sharing a pair of
// materials and all tagged into a SINGLE dynamic BatchGroup. The engine
// merges them into a handful of batched draw calls (one per material) instead
// of hundreds of individual draws. Because the group is DYNAMIC, every object
// can be moved every frame — the BatchManager rebuilds the per-frame matrix
// palette in Engine::update() (BatchManager::updateAll()).
//
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/batching/batchGroup.h"
#include "framework/batching/batchManager.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

constexpr int kBatchGroupId = 0;
constexpr int numInstances = 500;
constexpr float kCamDist = 70.0f;

class DynamicBatchingExample final: public ExampleApp
{
public:
    DynamicBatchingExample()
        : ExampleApp({.title = "Dynamic Batching Example", .width = 1100, .height = 750}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Dynamic Batching Example Started ***");

        // Upstream leaves every scene default in place: no environment atlas, black
        // ambient, linear tone mapping. The single directional light below (parented to
        // the camera) is the only illumination, which is what gives the primitives their
        // strong shaded/unshaded contrast against the flat clear colour.

        // -----------------------------------------------------------------------
        // Two shared materials — batching produces (at least) one draw call per
        // distinct material, so this scene resolves to ~2 batched draws.
        // -----------------------------------------------------------------------
        _material1 = std::make_shared<StandardMaterial>();
        _material1->setDiffuse(Color(1.0f, 1.0f, 0.0f));
        _material1->setGloss(0.4f);
        _material1->setMetalness(0.5f);
        _material1->setUseMetalness(true);

        _material2 = std::make_shared<StandardMaterial>();
        _material2->setDiffuse(Color(0.0f, 1.0f, 1.0f));
        _material2->setGloss(0.4f);
        _material2->setMetalness(0.5f);
        _material2->setUseMetalness(true);

        // -----------------------------------------------------------------------
        // Register a single DYNAMIC BatchGroup BEFORE creating the entities.
        // BatchGroup fields set: id, name, dynamic=true, maxAabbSize=100,
        // layers={} (empty → BatchManager defaults the batch to WORLD layer id 1,
        // which is what RenderComponent uses by default).
        //
        // Note: AppOptions::batchManager is left unset — Engine::init() creates a
        // default BatchManager, reachable via engine()->batcher().
        // -----------------------------------------------------------------------
        BatchGroup meshesGroup(kBatchGroupId, "Meshes", /*dynamic*/ true,
                               /*maxAabbSize*/ 100.0f, /*layers*/ {});
        engine()->batcher()->addGroup(meshesGroup);

        // -----------------------------------------------------------------------
        // Create many small procedural primitives, all tagged into the batch group.
        // -----------------------------------------------------------------------
        const std::vector<std::string> shapes = {"box", "cone", "cylinder", "sphere", "capsule"};

        std::mt19937 rng(1337u);
        std::uniform_int_distribution<int> shapeDist(0, static_cast<int>(shapes.size()) - 1);
        std::uniform_real_distribution<float> matDist(0.0f, 1.0f);

        _entities.reserve(numInstances);

        for (int i = 0; i < numInstances; ++i) {
            const std::string& shapeName = shapes[shapeDist(rng)];

            auto* entity = new Entity();
            entity->setEngine(engine());
            if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
                render->setType(shapeName);
                render->setMaterial(matDist(rng) < 0.5f ? _material1.get() : _material2.get());
                render->setCastShadows(true);

                // Tag into the batch group — the engine will try to render all of
                // these in a small number of draw calls (one per material).
                render->setBatchGroupId(kBatchGroupId);
            }
            root()->addChild(entity);
            _entities.push_back(entity);
        }

        // -----------------------------------------------------------------------
        // Ground box — NOT batched (its own draw), gives the moving meshes context.
        // Left as a caster, like upstream (RenderComponent::castShadows defaults to
        // true on both engines). The directional shadow camera fits its depth range
        // to CASTERS, so a receiver-only ground would sit outside that range and
        // catch no shadows at all.
        // -----------------------------------------------------------------------
        auto* ground = createPrimitive("box", _material2.get(), Vector3(0.0f, -26.0f, 0.0f),
            Vector3(150.0f, 1.0f, 150.0f));
        if (auto* groundRender = ground->findComponent<RenderComponent>()) {
            groundRender->setReceiveShadows(true);
        }

        // -----------------------------------------------------------------------
        // Camera — orbits the origin in the y = 0 plane, always looking at it.
        // -----------------------------------------------------------------------
        _camera = createCamera(Vector3(0.0f, 0.0f, kCamDist));
        if (auto* cameraComp = _camera->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
        }

        // Directional light parented to the camera so it rotates with the view.
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* lightComp = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lightComp->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            lightComp->setCastShadows(true);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.06f);
            lightComp->setShadowDistance(150.0f);
        }
        light->setLocalEulerAngles(15.0f, 30.0f, 0.0f);
        _camera->addChild(light);

        // -----------------------------------------------------------------------
        // Build the batches. prepare() merges geometry per (group, material),
        // hides the originals, and registers the batched MeshInstances with the
        // scene layers. Must run AFTER the tagged entities exist.
        // -----------------------------------------------------------------------
        engine()->batcher()->prepare(scene().get());

        spdlog::info("Dynamic batching: BatchGroup id={} name=\"{}\" dynamic={} maxAabbSize={} — "
                     "{} procedural primitives sharing 2 materials tagged into it.",
                     kBatchGroupId, "Meshes", true, 100.0f, numInstances);
        spdlog::info("These render as a handful of batched draws (one per material) rebuilt every "
                     "frame via BatchManager::updateAll().");

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        const float time = _time;

        // Move every batched entity along its own orbit — exercises DYNAMIC
        // batching (transforms change per frame, palette rebuilt in updateAll).
        for (int i = 0; i < numInstances; ++i) {
            const float radius = 5.0f + (20.0f * static_cast<float>(i)) / numInstances;
            const float speed = static_cast<float>(i) / numInstances;
            const float fi = static_cast<float>(i);
            _entities[i]->setLocalPosition(
                radius * std::sin(fi + time * speed),
                radius * std::cos(fi + time * speed),
                radius * std::cos(fi + 2.0f * time * speed)
            );
            _entities[i]->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        }

        // Orbit the camera around the scene.
        _camera->setLocalPosition(70.0f * std::sin(time), 0.0f, 70.0f * std::cos(time));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
    }

    void destroy() override
    {
        spdlog::info("*** Dynamic Batching Example Finished ***");
    }

private:
    std::shared_ptr<StandardMaterial> _material1;
    std::shared_ptr<StandardMaterial> _material2;
    std::vector<Entity*> _entities;
    Entity* _camera = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(DynamicBatchingExample)
