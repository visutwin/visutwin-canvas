// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Port of upstream particles/snow.
//
// A night scene lit only by the moon: a dark blue clear colour, a cool blue ambient and a
// cool directional moonlight (intensity 1.4, PCF3 shadows over 40 units at 2048, normal
// offset 0.05). Six hundred large snowflakes fall for ten seconds each from a 24 x 2 x 24
// box nine units above a snowy disc (a flattened cylinder 40 across), each at a random speed
// between 0.6 and 1.0, spinning at a random rate between -100 and 100 degrees a second and
// tinted slightly blue; the emitter is pre-warmed, so the air is full from the first frame.
// Seven pine trees stand around the emitter, each a cylinder trunk and three cone tiers with
// a slightly smaller, higher snow cone over each tier. Depth softening fades each flake where
// it meets the ground and the trees, which needs the camera's scene depth
// (requestSceneDepthMap). The camera frame tone-maps with ACES and adds a little bloom (0.03)
// and a strong vignette (0.7).
//
// DEVIATION: the window renders at one pixel per point (the examples harness), where upstream
// asks for up to two.
//
// Keys stand in for upstream's control panel:
//   S  soft particles on/off (depth softening 0.08 and the camera's depth map, together)
//
#include <array>
#include <memory>
#include <vector>

#include "extras/script/cameraControls.h"
#include "exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class ParticlesSnowExample final: public ExampleApp
{
public:
    ParticlesSnowExample(): ExampleApp({}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        _snowflake = std::make_unique<Asset>("snowflake", AssetType::TEXTURE, assetPath("textures/snowflake.png"));
        Texture* snowflake = _snowflake->resourceAs<Texture>();

        // Night scene: a dark blue sky lit only by the moon
        const Color nightColor(0.02f, 0.03f, 0.07f, 1.0f);
        scene()->setAmbientLight(0.06f, 0.08f, 0.14f);

        // The camera
        auto* camera = createCamera(Vector3(0.0f, 6.0f, 13.0f));
        _camera = camera->findComponent<CameraComponent>();
        if (_camera && _camera->camera()) {
            _camera->camera()->setClearColor(nightColor);
            _camera->camera()->setFov(50.0f);
        }

        // Tone mapping, bloom and vignette
        if (_camera) {
            _camera->setToneMapping(TONEMAP_ACES);
            auto rendering = _camera->rendering();
            rendering.bloomIntensity = 0.03f;
            rendering.vignetteEnabled = true;
            rendering.vignetteIntensity = 0.7f;
            _camera->setRendering(rendering);
        }

        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 2.5f, 0.0f))) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.01f, 190.0f));
            controls->storeResetState();
        }

        // Cool moonlight with shadows
        auto* moon = createDirectionalLight(Vector3(55.0f, 150.0f, 0.0f), Color(0.55f, 0.7f, 1.0f, 1.0f), 1.4f, true);
        moon->setName("moon");
        if (auto* light = moon->findComponent<LightComponent>()) {
            light->setShadowType(SHADOW_PCF3_32F);
            light->setShadowDistance(40.0f);
            light->setShadowResolution(2048);
            light->setShadowNormalBias(0.05f);
        }

        // Materials
        _snowMaterial = std::make_shared<StandardMaterial>();
        _snowMaterial->setDiffuse(Color(0.9f, 0.93f, 1.0f, 1.0f));
        _snowMaterial->setGloss(0.3f);
        _snowMaterial->setUseMetalness(true);
        _snowMaterial->setMetalness(0.0f);

        _needleMaterial = std::make_shared<StandardMaterial>();
        _needleMaterial->setDiffuse(Color(0.08f, 0.2f, 0.14f, 1.0f));
        _needleMaterial->setUseMetalness(true);
        _needleMaterial->setMetalness(0.0f);
        _needleMaterial->setGloss(0.2f);

        _trunkMaterial = std::make_shared<StandardMaterial>();
        _trunkMaterial->setDiffuse(Color(0.2f, 0.12f, 0.08f, 1.0f));
        _trunkMaterial->setUseMetalness(true);
        _trunkMaterial->setMetalness(0.0f);

        // The particle system
        auto* entity = new Entity();
        entity->setEngine(engine());
        root()->addChild(entity);
        entity->setLocalPosition(0.0f, 9.0f, 0.0f);
        _particles = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());
        auto& o = _particles->options();
        o.numParticles = 600;
        o.lifetime = o.lifetime2 = 10.0f;
        o.rate = 0.0167f;
        o.startAngle = 360.0f;
        o.startAngle2 = -360.0f;
        o.emitterExtents = Vector3(24.0f, 2.0f, 24.0f);
        // A random downward speed from -0.6 to -1.0.
        o.velocityGraph = CurveSet({{0.0f, 0.0f}, {0.0f, -1.0f}, {0.0f, 0.0f}});
        o.velocityGraph2 = CurveSet({{0.0f, 0.0f}, {0.0f, -0.6f}, {0.0f, 0.0f}});
        o.scaleGraph = Curve({0.0f, 0.2f});
        // A random rotation speed from -100 to 100 degrees a second.
        o.rotationSpeedGraph = Curve({0.0f, 100.0f});
        o.rotationSpeedGraph2 = Curve({0.0f, -100.0f});
        // A slight blue tint, constant over the lifetime.
        o.colorGraph = CurveSet({{0.0f, 0.72f}, {0.0f, 0.84f}, {0.0f, 1.0f}});
        o.colorMap = snowflake;
        o.preWarm = true;
        o.depthSoftening = 0.08f;
        _particles->apply();
        _particles->play();

        // Snowy ground
        auto* ground = createPrimitive("cylinder", _snowMaterial.get(), Vector3(0.0f, -0.1f, 0.0f),
            Vector3(40.0f, 0.2f, 40.0f));
        ground->setName("ground");
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setReceiveShadows(true);
        }

        // A few pine trees made from cones, with snow caps, scattered around the emitter:
        // x, z and size.
        constexpr std::array<std::array<float, 3>, 7> treePositions = {{
            {-6.0f, -4.0f, 1.3f},
            {5.5f, -5.0f, 1.6f},
            {-4.5f, 3.0f, 1.0f},
            {7.0f, 2.5f, 1.2f},
            {-8.5f, -1.0f, 1.5f},
            {1.0f, -8.0f, 1.8f},
            {3.5f, 5.5f, 0.9f},
        }};
        for (const auto& [x, z, size] : treePositions) {
            auto* tree = new Entity();
            tree->setEngine(engine());
            tree->setName("tree");
            tree->setLocalPosition(x, 0.0f, z);
            root()->addChild(tree);

            addPart(tree, "trunk", "cylinder", _trunkMaterial.get(), true, true,
                Vector3(0.0f, 0.5f * size, 0.0f), Vector3(0.4f * size, 1.0f * size, 0.4f * size));

            for (int i = 0; i < 3; ++i) {
                const float s = static_cast<float>(3 - i) * 0.9f * size;
                const float y = (1.4f + static_cast<float>(i) * 1.2f) * size;

                addPart(tree, "needles", "cone", _needleMaterial.get(), true, true,
                    Vector3(0.0f, y + size, 0.0f), Vector3(s, 2.0f * size, s));

                // Slightly smaller, higher snow cone on top of each tier
                addPart(tree, "snow", "cone", _snowMaterial.get(), true, true,
                    Vector3(0.0f, y + size + 0.35f * size, 0.0f), Vector3(s * 0.82f, 1.5f * size, s * 0.82f));
            }
        }

        applySoft();
        spdlog::info("Keys: S soft particles");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_S) {
            _soft = !_soft;
            applySoft();
            return true;
        }
        return false;
    }

private:
    // A primitive child of `parent`, placed in the parent's space.
    void addPart(Entity* parent, const char* name, const char* type, Material* material, const bool castShadows,
        const bool receiveShadows, const Vector3& position, const Vector3& scale)
    {
        auto* part = new Entity();
        part->setEngine(engine());
        part->setName(name);
        if (auto* render = static_cast<RenderComponent*>(part->addComponent<RenderComponent>())) {
            render->setMaterial(material);
            render->setType(type);
            render->setCastShadows(castShadows);
            render->setReceiveShadows(receiveShadows);
        }
        part->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
        part->setLocalPosition(position);
        parent->addChild(part);
    }

    // The depth softening and the camera's depth map toggle together.
    void applySoft()
    {
        if (_particles) {
            _particles->options().depthSoftening = _soft ? 0.08f : 0.0f;
            _particles->apply();
        }
        // The request is counted: ask once, and release only what was asked.
        if (_camera && _soft != _depthRequested) {
            _camera->requestSceneDepthMap(_soft);
            _depthRequested = _soft;
        }
        spdlog::info("soft particles {}", _soft);
    }

    std::unique_ptr<Asset> _snowflake;
    std::shared_ptr<StandardMaterial> _snowMaterial;
    std::shared_ptr<StandardMaterial> _needleMaterial;
    std::shared_ptr<StandardMaterial> _trunkMaterial;
    CameraComponent* _camera = nullptr;
    ParticleSystemComponent* _particles = nullptr;
    bool _soft = true;
    bool _depthRequested = false;
};

VISUTWIN_EXAMPLE_MAIN(ParticlesSnowExample)
