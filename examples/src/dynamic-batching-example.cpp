// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026
//
// Port of upstream graphics/batching-dynamic.
//
// A kinetic sculpture: about 700 small metal shapes float in a hexagonal grid filling a circle
// of radius 4, in rings of gold, silver and copper from the centre out, rising and falling in
// patterns - a wave, a ripple, a spiral, a saddle and a dome - each lasting 9 seconds, the last 3
// blending into the next, every shape spinning slowly. All of them are in one DYNAMIC batch group
// (split above 20 units, which the sculpture never reaches), so they render in a handful of draw
// calls while moving every frame. The studio environment atlas lights the metal, though the
// camera (fov 55, ACES) renders no skybox: the sculpture floats in a dark room, its floor fading
// into the black through linear fog (14 - 34). A sun with a shadow fitted to the sculpture's
// bounds shines from above.
//
// Keys stand in for upstream's controls panel:
//   B            batching on or off (off: one draw call per shape)
//   P            the pattern: cycle, wave, ripple, spiral, saddle, dome
//   S            the shape: sphere, box, cone, capsule, cylinder, mixed
//   , / .        the speed -/+ 0.25 (0 - 3)
//
// DEVIATIONS:
// - Each shape's starting spin comes from a fixed-seed generator (mulberry32, seed 1) rather than
//   Math.random(), so every run is the same.
// - The draw-call count is the performance HUD's (upstream shows it in its panel).
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/batching/batchGroup.h"
#include "framework/batching/batchManager.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kBatchGroupId = 0;
    constexpr float kRadius = 4.0f;
    constexpr float kSpacing = 0.28f;
    constexpr float kPatternDuration = 9.0f;
    constexpr float kBlendDuration = 3.0f;

    struct ShapeInfo
    {
        const char* type;
        Vector3 scale;
        float tilt;   // the cone points down
    };
    const std::array<ShapeInfo, 5> kShapes = {{
        {"sphere", Vector3(0.22f, 0.22f, 0.22f), 0.0f},
        {"box", Vector3(0.17f, 0.17f, 0.17f), 0.0f},
        {"cone", Vector3(0.2f, 0.3f, 0.2f), 180.0f},
        {"capsule", Vector3(0.13f, 0.15f, 0.13f), 0.0f},
        {"cylinder", Vector3(0.18f, 0.22f, 0.18f), 0.0f},
    }};

    struct Item
    {
        float x = 0.0f;
        float z = 0.0f;
        float r = 0.0f;       // distance from the centre
        float a = 0.0f;       // angle around it
        float spin = 0.0f;
        int mixedShape = 0;   // the shape when the shapes are mixed
        Entity* entity = nullptr;
        float tilt = 0.0f;
    };

    // The patterns, each giving a shape's height between -1 and 1.
    using Pattern = float (*)(const Item&, float);
    const std::array<Pattern, 5> kPatterns = {
        [](const Item& item, const float t) { return std::sin(item.x * 1.1f - t * 1.4f); },
        [](const Item& item, const float t) { return std::sin(item.r * 2.2f - t * 1.8f); },
        [](const Item& item, const float t) { return std::sin(item.a * 3.0f - item.r * 1.4f + t * 1.2f); },
        [](const Item& item, const float t) {
            return std::sin(t * 0.9f) * (item.x * item.x - item.z * item.z) / (kRadius * kRadius);
        },
        [](const Item& item, const float t) {
            return std::cos(t * 0.8f) * (1.0f - 2.0f * item.r * item.r / (kRadius * kRadius));
        },
    };
    const std::array<const char*, 6> kPatternNames = {"wave", "ripple", "spiral", "saddle", "dome", "cycle"};

    float smoothstep(const float edge0, const float edge1, const float x)
    {
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
}

class DynamicBatchingExample final : public ExampleApp
{
public:
    DynamicBatchingExample() : ExampleApp({.title = "Dynamic Batching"}) {}

protected:
    bool create() override
    {
        _studio = std::make_unique<Asset>("studio-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (Texture* studio = _studio->resourceAs<Texture>()) {
            scene()->setEnvAtlas(studio);
        }
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_LINEAR);
        scene()->setFogColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        scene()->setFogLinear(14.0f, 34.0f);

        // Gold, silver and copper, in rings from the centre out.
        _metals = {createMaterial(Color(1.0f, 0.77f, 0.34f, 1.0f), 0.85f, 1.0f),
            createMaterial(Color(0.95f, 0.95f, 0.95f, 1.0f), 0.9f, 1.0f),
            createMaterial(Color(0.95f, 0.55f, 0.4f, 1.0f), 0.85f, 1.0f)};

        // The floor.
        _floorMaterial = createMaterial(Color(0.07f, 0.07f, 0.07f, 1.0f), 0.3f, 0.0f);
        Entity* floor = createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(100.0f, 1.0f, 100.0f));
        if (auto* render = floor->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        // A dynamic batch group, so its meshes can move every frame.
        engine()->batcher()->addGroup(BatchGroup(kBatchGroupId, "Sculpture", true, 20.0f, {}));

        // The shapes in a hexagonal grid filling a circle.
        uint32_t seed = 1;
        const auto unit = [&seed] {
            // mulberry32
            seed += 0x6D2B79F5u;
            uint32_t t = (seed ^ (seed >> 15)) * (seed | 1u);
            t = (t + ((t ^ (t >> 7)) * (t | 61u))) ^ t;
            return static_cast<float>(static_cast<double>(t ^ (t >> 14)) / 4294967296.0);
        };
        const float rowSpacing = kSpacing * std::sqrt(3.0f) / 2.0f;
        const int rows = static_cast<int>(std::ceil(kRadius / rowSpacing));
        const int columns = static_cast<int>(std::ceil(kRadius / kSpacing)) + 1;
        for (int row = -rows; row <= rows; ++row) {
            for (int column = -columns; column <= columns; ++column) {
                const float x = (static_cast<float>(column) + static_cast<float>(row & 1) * 0.5f) * kSpacing;
                const float z = static_cast<float>(row) * rowSpacing;
                const float r = std::sqrt(x * x + z * z);
                if (r > kRadius) {
                    continue;
                }
                const size_t ring = std::min(static_cast<size_t>(r / kRadius * _metals.size()), _metals.size() - 1);
                Item item;
                item.x = x;
                item.z = z;
                item.r = r;
                item.a = std::atan2(z, x);
                item.spin = unit() * 360.0f;
                item.mixedShape = static_cast<int>(_items.size() % kShapes.size());
                item.entity = createPrimitive("sphere", _metals[ring].get());
                _items.push_back(item);
            }
        }

        // The sun, its shadow fitted to the sculpture and the floor around it.
        auto* sun = new Entity();
        sun->setName("Sun");
        sun->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(sun->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            light->setColor(Color(1.0f, 0.92f, 0.8f, 1.0f));
            light->setIntensity(1.5f);
            light->setCastShadows(true);
            light->setShadowType(SHADOW_PCF3_32F);
            light->setShadowResolution(2048);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.05f);
            light->setShadowBounds(BoundingBox(Vector3(0.0f, 2.8f, 0.0f), Vector3(7.0f, 2.8f, 7.0f)));
        }
        sun->setLocalEulerAngles(50.0f, 30.0f, 0.0f);
        root()->addChild(sun);

        // The camera renders no skybox, so the studio is not seen.
        Entity* camera = createCamera(Vector3(0.0f, 4.8f, 10.0f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_IMMEDIATE, LAYERID_UI});
        cameraComponent->setToneMapping(TONEMAP_ACES);
        cameraComponent->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        cameraComponent->camera()->setFov(55.0f);
        cameraComponent->camera()->setNearClip(0.1f);
        cameraComponent->camera()->setFarClip(100.0f);
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 2.6f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-70.0f, 10.0f));
            controls->setZoomRange(Vector2(2.0f, 18.0f));
            controls->storeResetState();
        }

        applySettings();
        update(0.0f);
        engine()->batcher()->prepare(scene().get());
        spdlog::info("dynamic-batching: {} shapes; B batching, P pattern, S shape, , . speed", _items.size());
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_B: _batching = !_batching; applySettings(); break;
        case SDLK_P: _pattern = (_pattern + 1) % static_cast<int>(kPatternNames.size());
            spdlog::info("pattern: {}", kPatternNames[static_cast<size_t>(_pattern)]); break;
        case SDLK_S: _shape = (_shape + 1) % static_cast<int>(kShapes.size() + 1); applySettings(); break;
        case SDLK_COMMA: _speed = std::max(0.0f, _speed - 0.25f); break;
        case SDLK_PERIOD: _speed = std::min(3.0f, _speed + 0.25f); break;
        default: return false;
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * _speed;

        // In the cycle, each pattern lasts a while, its last seconds blending into the next.
        constexpr int kCycle = 5;
        Pattern from = _pattern == kCycle ? nullptr : kPatterns[static_cast<size_t>(_pattern)];
        Pattern to = from;
        float blend = 0.0f;
        if (_pattern == kCycle) {
            const int index = static_cast<int>(std::floor(_time / kPatternDuration));
            from = kPatterns[static_cast<size_t>(index) % kPatterns.size()];
            to = kPatterns[static_cast<size_t>(index + 1) % kPatterns.size()];
            const float elapsed = _time - static_cast<float>(index) * kPatternDuration;
            blend = smoothstep(kPatternDuration - kBlendDuration, kPatternDuration, elapsed);
        }

        // Each shape at its height, spinning slowly.
        for (const auto& item : _items) {
            const float a = from(item, _time);
            const float height = a + (to(item, _time) - a) * blend;
            item.entity->setLocalPosition(item.x, 2.8f + height * 1.3f, item.z);
            item.entity->setLocalEulerAngles(item.tilt, item.spin + _time * 30.0f, 0.0f);
        }
    }

private:
    std::shared_ptr<StandardMaterial> createMaterial(const Color& color, const float gloss, const float metalness)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setGloss(gloss);
        material->setUseMetalness(true);
        material->setMetalness(metalness);
        return material;
    }

    // The shapes take the chosen one (a new shape enters the batches again), and the batch
    // group, or none: without it each shape is a draw call of its own.
    void applySettings()
    {
        for (auto& item : _items) {
            const int shape = _shape == static_cast<int>(kShapes.size()) ? item.mixedShape : _shape;
            const ShapeInfo& info = kShapes[static_cast<size_t>(shape)];
            if (auto* render = item.entity->findComponent<RenderComponent>()) {
                render->setType(info.type);
                render->setCastShadows(true);
                render->setBatchGroupId(_batching ? kBatchGroupId : -1);
            }
            item.entity->setLocalScale(info.scale.getX(), info.scale.getY(), info.scale.getZ());
            item.tilt = info.tilt;
        }
    }

    std::unique_ptr<Asset> _studio;
    std::vector<std::shared_ptr<StandardMaterial>> _metals;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::vector<Item> _items;
    bool _batching = true;
    int _pattern = 5;   // cycle
    int _shape = 0;     // sphere
    float _speed = 1.0f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(DynamicBatchingExample)
