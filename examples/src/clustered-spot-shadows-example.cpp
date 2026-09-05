// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/clustered-spot-shadows.
//
// Ten cookie-projecting spot lights circle a field of tumbled cubes, every one of
// them casting a real shadow. Without clustered lighting the engine can only carry
// a handful of shadow-casting local lights at once; with it, each spot renders into
// its own slice of the local shadow atlas and the clustered fragment shader samples
// the slice the light says it owns. This is the only example that exercises that
// atlas.
//
// DEVIATIONS from upstream: this port's atlas is a texture ARRAY of full-resolution
// slices rather than one packed 2D atlas, so upstream's atlas split options have no
// equivalent and the setting is a per-slice resolution plus a capacity; the cookie
// channel is not varied per light; and upstream's controls panel, static-light mode
// and atlas debug overlay are left out.
//
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kLightCount = 10;
    constexpr int kTowers = 8;
    constexpr float PI_F = 3.14159265358979323846f;
}

class ClusteredSpotShadowsExample final: public ExampleApp
{
public:
    ClusteredSpotShadowsExample()
        : ExampleApp({.title = "Clustered Spot Shadows"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setSkyboxMip(3);
        scene()->setSkyboxIntensity(0.1f);

        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (const auto env = _envAtlas->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*env));
        }

        _normalMap = std::make_unique<Asset>("normal", AssetType::TEXTURE,
            assetPath("textures/normal-map.png"));
        _cookie = std::make_unique<Asset>("heart", AssetType::TEXTURE,
            assetPath("textures/heart.png"));

        // Clustered lighting, sized for a scene where every light overlaps the
        // middle: a coarse grid but a generous per-cell budget.
        scene()->setClusteredLightingEnabled(true);
        auto& lighting = scene()->lighting();
        lighting.cellsX = 12;
        lighting.cellsY = 4;
        lighting.cellsZ = 12;
        lighting.maxLightsPerCell = 24;
        lighting.shadowsEnabled = true;
        lighting.cookiesEnabled = true;
        lighting.shadowAtlasResolution = 512;
        lighting.shadowAtlasCapacity = kLightCount;

        auto groundMaterial = std::make_shared<StandardMaterial>();
        groundMaterial->setGloss(0.55f);
        groundMaterial->setMetalness(0.4f);
        groundMaterial->setUseMetalness(true);
        groundMaterial->setNormalMap(textureOf(_normalMap));
        groundMaterial->setNormalMapTiling(Vector2(10.0f, 10.0f));
        groundMaterial->setBumpiness(0.5f);
        _materials.push_back(groundMaterial);

        auto cubeMaterial = std::make_shared<StandardMaterial>();
        cubeMaterial->setGloss(0.55f);
        cubeMaterial->setMetalness(0.4f);
        cubeMaterial->setUseMetalness(true);
        cubeMaterial->setNormalMap(textureOf(_normalMap));
        cubeMaterial->setNormalMapTiling(Vector2(0.25f, 0.25f));
        cubeMaterial->setBumpiness(0.5f);
        _materials.push_back(cubeMaterial);

        createPrimitive("box", groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(500.0f, 1.0f, 500.0f));

        // Eight towers of tumbled cubes on a circle, tapering inward with height.
        std::uniform_real_distribution<float> angle(0.0f, 360.0f);
        for (int i = 0; i < kTowers; ++i) {
            const float fraction = (static_cast<float>(i) / kTowers) * PI_F * 2.0f;
            constexpr float radius = 200.0f;
            constexpr int numCubes = 12;
            for (int y = 0; y <= 10; ++y) {
                const float elevationRadius =
                    radius * (1.0f - static_cast<float>(y) / numCubes);
                Entity* cube = createPrimitive("box", cubeMaterial.get(),
                    Vector3(elevationRadius * std::sin(fraction),
                        static_cast<float>(y) * 6.0f,
                        elevationRadius * std::cos(fraction)),
                    Vector3(12.0f, 12.0f, 12.0f));
                cube->setLocalEulerAngles(angle(_rng), angle(_rng), angle(_rng));
            }
        }

        std::uniform_real_distribution<float> channel(0.0f, 1.5f);
        for (int i = 0; i < kLightCount; ++i) {
            createSpotLight(i, Color(channel(_rng), channel(_rng), channel(_rng), 1.0f));
        }

        auto* camera = createCamera(Vector3(0.0f, 150.0f, 300.0f));
        if (auto* comp = camera->findComponent<CameraComponent>();
            comp != nullptr && comp->camera() != nullptr) {
            comp->camera()->setNearClip(1.0f);
            comp->camera()->setFarClip(2000.0f);
            comp->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
        }
        auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f));
        controls->setOrbitDistance(335.0f);
        controls->setMoveSpeed(200.0f);
        controls->storeResetState();

        spdlog::info("{} shadow-casting spot lights, each in its own slice of the "
                     "local shadow atlas.", kLightCount);
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * 0.15f;
        for (size_t i = 0; i < _lights.size(); ++i) {
            const float angle =
                (static_cast<float>(i) / static_cast<float>(_lights.size())) * PI_F * 2.0f;
            const float x = 130.0f * std::sin(angle + _time);
            const float z = 130.0f * std::cos(angle + _time);
            Entity* light = _lights[i];
            light->setLocalPosition(x, 100.0f, z);
            // A light shines down its entity's -Y axis, so aiming it is lookAt
            // (which orients -Z) plus a quarter turn, the same as every spot in
            // this port.
            light->lookAt(Vector3(x, 0.0f, z), Vector3(1.0f, 0.0f, 0.0f));
            light->rotateLocal(90.0f, 0.0f, 0.0f);
        }
    }

private:
    static Texture* textureOf(const std::unique_ptr<Asset>& asset)
    {
        const auto resource = asset->resource();
        if (!resource || !std::holds_alternative<Texture*>(*resource)) {
            return nullptr;
        }
        return std::get<Texture*>(*resource);
    }

    void createSpotLight(const int index, const Color& color)
    {
        auto* entity = new Entity();
        entity->setName("Spot-" + std::to_string(index));
        entity->setEngine(engine());

        if (auto* light = static_cast<LightComponent*>(
                entity->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_SPOT);
            light->setColor(color);
            light->setIntensity(3.0f);
            light->setInnerConeAngle(30.0f);
            light->setOuterConeAngle(35.0f);
            light->setRange(150.0f);
            light->setCastShadows(true);
            light->setShadowBias(0.4f);
            light->setShadowNormalBias(0.1f);
            light->setShadowResolution(512);
            light->setCookie(textureOf(_cookie));
            light->setCookieIntensity(0.5f);
        }

        // A small emissive cone marks where each light is.
        auto material = std::make_shared<StandardMaterial>();
        material->setEmissive(color);
        _materials.push_back(material);
        if (auto* render = static_cast<RenderComponent*>(
                entity->addComponent<RenderComponent>())) {
            render->setMaterial(material.get());
            render->setType("cone");
            render->setCastShadows(false);
        }

        entity->setLocalScale(5.0f, 5.0f, 5.0f);
        root()->addChild(entity);
        _lights.push_back(entity);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _normalMap;
    std::unique_ptr<Asset> _cookie;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _lights;
    std::mt19937 _rng{20260905};
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ClusteredSpotShadowsExample)
