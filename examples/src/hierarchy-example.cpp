// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/hierarchy.
//
// A deep entity hierarchy: under one dummy root, six levels of 2x2 grids of boxes and spheres,
// each entity parented to the one it grows out of, every level smaller (scale 1.7, less 0.25 a
// level) and closer together (spacing 7, times 0.7 a level) than the one before - 5460 entities,
// each with its own StandardMaterial of a random diffuse colour. Every frame each entity's local
// rotation is set to the same time-driven rotation, so the rotations compound down the
// hierarchy. One omni light (range 150) and an ambient of 0.2 light it; the camera is fixed.
//
// DEVIATION: upstream picks the shapes and colours with Math.random; this port draws them from
// a fixed-seed std::mt19937 in the same order, so the scene is the same in every run.
//
#include <memory>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class HierarchyExample final: public ExampleApp
{
public:
    HierarchyExample(): ExampleApp({.title = "Hierarchy"}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // Dummy root entity
        auto* hierarchyRoot = new Entity();
        hierarchyRoot->setEngine(engine());
        root()->addChild(hierarchyRoot);

        // Generate the hierarchy of child entities
        constexpr int levels = 5;
        constexpr int gridSize = 2;
        constexpr float scale = 1.7f;
        constexpr float scaleDelta = 0.25f;
        constexpr float spacing = 7.0f;
        createChildren(hierarchyRoot, gridSize, scale, scaleDelta, spacing, levels);
        spdlog::info("number of created entities: {}", _entities.size());

        // The main camera
        auto* camera = createCamera(Vector3(0.0f, 40.0f, 90.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        }
        camera->lookAt(Vector3(0.0f, 5.0f, 0.0f));

        // An omni light
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            component->setType(LightType::LIGHTTYPE_OMNI);
            component->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            component->setRange(150.0f);
        }
        light->setLocalPosition(40.0f, 60.0f, 50.0f);
        root()->addChild(light);

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // One rotation changing with time, applied to every entity
        const Quaternion rotation = Quaternion::fromEulerAngles(_time * 5.0f, _time * 13.0f, _time * 6.0f);
        for (Entity* entity : _entities) {
            entity->setLocalRotation(rotation);
        }
    }

private:
    // A primitive of the given shape, with a material of its own of a random colour
    Entity* createShape(const char* type, const Vector3& position, const float scale)
    {
        auto material = std::make_shared<StandardMaterial>();
        const float r = _random(_rng);
        const float g = _random(_rng);
        const float b = _random(_rng);
        material->setDiffuse(Color(r, g, b, 1.0f));
        _materials.push_back(material);

        auto* entity = new Entity();
        entity->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
            render->setMaterial(material.get());
            render->setType(type);
        }
        entity->setLocalPosition(position);
        entity->setLocalScale(scale, scale, scale);
        return entity;
    }

    // The next level of entities under `parent`, and recursively the levels under them
    void createChildren(Entity* parent, const int gridSize, const float scale, const float scaleDelta,
        const float spacing, const int levels)
    {
        if (levels < 0) {
            return;
        }
        const float offset = spacing * static_cast<float>(gridSize - 1) * 0.5f;
        for (int x = 0; x < gridSize; ++x) {
            for (int y = 0; y < gridSize; ++y) {
                const char* shape = _random(_rng) < 0.5f ? "box" : "sphere";
                const Vector3 position(static_cast<float>(x) * spacing - offset, spacing,
                    static_cast<float>(y) * spacing - offset);
                Entity* entity = createShape(shape, position, scale);

                parent->addChild(entity);
                _entities.push_back(entity);

                createChildren(entity, gridSize, scale - scaleDelta, scaleDelta, spacing * 0.7f, levels - 1);
            }
        }
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _entities;
    std::mt19937 _rng{2026u};
    std::uniform_real_distribution<float> _random{0.0f, 1.0f};
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(HierarchyExample)
