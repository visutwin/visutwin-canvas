// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Port of upstream graphics/particles-snow.
//
// A hundred snowflakes fall for ten seconds each from a 14 x 4 x 14 box five units above
// a flat disc, each at a random speed between 0.4 and 0.7 and spinning at a random rate
// between -100 and 100 degrees a second. Depth softening fades a flake as it nears the
// ground, which needs the camera's scene depth (requestSceneDepthMap).
//
// Keys stand in for upstream's control panel:
//   S  soft particles on/off (depth softening 0.08 and the camera's depth map, together)
//
#include <memory>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class ParticlesSnowExample final: public ExampleApp
{
public:
    ParticlesSnowExample(): ExampleApp({.title = "Particles: Snow"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        _snowflake = std::make_unique<Asset>("snowflake", AssetType::TEXTURE, assetPath("textures/snowflake.png"));
        Texture* snowflake = _snowflake->resourceAs<Texture>();

        auto* camera = createCamera(Vector3(0.0f, 7.0f, 10.0f));
        _camera = camera->findComponent<CameraComponent>();
        if (_camera && _camera->camera()) {
            _camera->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setZoomRange(Vector2(0.0f, 190.0f));
            controls->storeResetState();
        }

        createDirectionalLight(Vector3(45.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, false);

        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setLocalPosition(0.0f, 5.0f, 0.0f);
        root()->addChild(entity);
        _particles = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());
        auto& o = _particles->options();
        o.numParticles = 100;
        o.lifetime = o.lifetime2 = 10.0f;
        o.rate = 0.1f;
        o.startAngle = 360.0f;
        o.startAngle2 = -360.0f;
        o.emitterExtents = Vector3(7.0f, 2.0f, 7.0f);
        // A random downward speed from -0.4 to -0.7.
        o.velocityGraph = CurveSet({{0.0f, 0.0f}, {0.0f, -0.7f}, {0.0f, 0.0f}});
        o.velocityGraph2 = CurveSet({{0.0f, 0.0f}, {0.0f, -0.4f}, {0.0f, 0.0f}});
        o.scaleGraph = Curve({0.0f, 0.2f});
        // A random rotation speed from -100 to 100 degrees a second.
        o.rotationSpeedGraph = Curve({0.0f, 100.0f});
        o.rotationSpeedGraph2 = Curve({0.0f, -100.0f});
        o.colorMap = snowflake;
        _particles->apply();
        _particles->play();

        // The ground
        auto* ground = new Entity();
        ground->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>())) {
            render->setType("cylinder");
        }
        ground->setLocalScale(10.0f, 0.01f, 10.0f);
        root()->addChild(ground);

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
    CameraComponent* _camera = nullptr;
    ParticleSystemComponent* _particles = nullptr;
    bool _soft = true;
    bool _depthRequested = false;
};

VISUTWIN_EXAMPLE_MAIN(ParticlesSnowExample)
