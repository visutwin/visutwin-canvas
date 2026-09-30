// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/particles-spark.
//
// A fountain of sparks: each spark flies out in a random direction between two local velocity
// curves, is pulled down by a world velocity curve that turns into increasing gravity, spins at
// 360 degrees a second, grows and shrinks, and cools from yellow through orange to red.
//
// DEVIATION: upstream's box emitter adds its default initialVelocity of 1 along the emitter's -Z
// (particleUpdaterAABB). This port's initialVelocity is a vector, so it is written out as
// (0, 0, -1). Upstream also builds a localPosCurve that it never uses; it is left out.
//
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "core/math/curve.h"
#include "core/math/curveSet.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"

using namespace visutwin::canvas;

class ParticlesSparkExample final: public ExampleApp
{
public:
    ParticlesSparkExample(): ExampleApp({.title = "Particles: Spark"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        _spark = std::make_unique<Asset>("spark", AssetType::TEXTURE, assetPath("textures/spark.png"));
        Texture* spark = nullptr;
        if (const auto resource = _spark->resource(); resource && std::holds_alternative<Texture*>(*resource)) {
            spark = std::get<Texture*>(*resource);
        }
        if (spark == nullptr) {
            spdlog::error("particles-spark needs textures/spark.png");
            return false;
        }

        // Create an Entity with a camera component
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 10.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.0f, 0.0f, 0.05f, 1.0f));

        // Create a directional light
        createDirectionalLight(Vector3(45.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, false);

        // Create entity for particle system
        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setName("Sparks");
        root()->addChild(entity);
        entity->setLocalPosition(0.0f, 0.0f, 0.0f);

        auto* particles = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());
        auto& o = particles->options();
        o.numParticles = 200;
        o.lifetime = 2.0f;
        o.lifetime2 = 2.0f;
        o.rate = 0.01f;
        // Gradually make sparks bigger
        o.scaleGraph = Curve(std::vector<float>{0.0f, 0.0f, 0.5f, 0.3f, 0.8f, 0.2f, 1.0f, 0.1f});
        // Rotate sparks 360 degrees per second
        o.rotationSpeedGraph = Curve(std::vector<float>{0.0f, 360.0f});
        // Color changes throughout lifetime
        o.colorGraph = CurveSet({{0.0f, 1.0f, 0.25f, 1.0f, 0.375f, 0.5f, 0.5f, 0.0f},
                                 {0.0f, 0.0f, 0.125f, 0.25f, 0.25f, 0.5f, 0.375f, 0.75f, 0.5f, 1.0f},
                                 {0.0f, 0.0f, 1.0f, 0.0f}});
        o.colorMap = spark;
        // Increasing gravity
        o.velocityGraph = CurveSet({{0.0f, 0.0f}, {0.0f, 0.0f, 0.2f, 6.0f, 1.0f, -48.0f}, {0.0f, 0.0f}});
        // Make particles move in different directions
        o.localVelocityGraph = CurveSet({{0.0f, 0.0f, 1.0f, 8.0f}, {0.0f, 0.0f, 1.0f, 6.0f}, {0.0f, 0.0f, 1.0f, 0.0f}});
        o.localVelocityGraph2 = CurveSet({{0.0f, 0.0f, 1.0f, -8.0f}, {0.0f, 0.0f, 1.0f, -6.0f}, {0.0f, 0.0f, 1.0f, 0.0f}});
        // The default initialVelocity of upstream's box emitter, see the header
        o.initialVelocity = Vector3(0.0f, 0.0f, -1.0f);
        particles->apply();
        return true;
    }

private:
    std::unique_ptr<Asset> _spark;
};

VISUTWIN_EXAMPLE_MAIN(ParticlesSparkExample)
