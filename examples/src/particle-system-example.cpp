// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The built-in GPU particle system: ParticleSystemComponent driving a
// ParticleEmitter, which simulates on the GPU and draws camera-facing billboards.
//
// This is the first example of that component. Everything under it — the emitter,
// the compute simulation step, the curve-driven size/colour/alpha LUTs, the
// sprite-sheet animation — already existed but had no consumer, and a component
// system only ticks if the APPLICATION registers it (AppOptions::componentSystems).
// Without the registerComponentSystem call below, the component is constructed and
// then never updated, so the emitter never simulates and nothing moves.
//
// Two emitters, so the shape and blend options are both visible:
//   * a box-shaped additive fountain using the numbered sprite sheet, which also
//     exercises the sprite animation path;
//   * a sphere-shaped alpha-blended puff with gravity and damping.
//
#include <memory>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class ParticleSystemExample final : public ExampleApp
{
public:
    ParticleSystemExample() : ExampleApp({.title = "Particle System"}) {}

protected:
    void configure(AppOptions& options) override
    {
        // Without this the component is inert: nothing else ticks it.
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.1f, 0.12f, 0.16f);

        _sprite = std::make_unique<Asset>(
            "particle-sprite", AssetType::TEXTURE,
            assetPath("textures/particles-numbers.png"));
        Texture* spriteTexture = nullptr;
        if (const auto resource = _sprite->resource()) {
            spriteTexture = std::get<Texture*>(*resource);
        }

        // Additive fountain: a box emitter throwing sprites upward.
        {
            auto* entity = new Entity();
            entity->setEngine(engine());
            entity->setLocalPosition(-1.2f, 0.0f, 0.0f);
            root()->addChild(entity);

            if (auto* particles = static_cast<ParticleSystemComponent*>(
                    entity->addComponent<ParticleSystemComponent>())) {
                auto& o = particles->options();
                o.numParticles = 1024;
                o.lifetime = 1.5f;
                o.lifetime2 = 2.5f;
                o.loop = true;
                o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_BOX;
                o.emitterExtents = Vector3(0.15f, 0.0f, 0.15f);
                o.initialVelocity = Vector3(0.0f, 2.2f, 0.0f);
                o.velocitySpread = Vector3(0.5f, 0.4f, 0.5f);
                o.gravity = Vector3(0.0f, -1.6f, 0.0f);
                o.rotationSpeed = -90.0f;
                o.rotationSpeed2 = 90.0f;
                o.intensity = 2.0f;
                o.blendType = ParticleBlendType::BLEND_ADDITIVE;

                // Sprite sheet: 512x512 of 4x4 numbered tiles, played once per life.
                o.colorMap = spriteTexture;
                o.animTilesX = 4;
                o.animTilesY = 4;
                o.animNumFrames = 16;
                o.animSpeed = 1.0f;

                // Grow then shrink, fading out over the second half.
                o.scaleGraph = Curve();
                o.scaleGraph.add(0.0f, 0.05f);
                o.scaleGraph.add(0.3f, 0.18f);
                o.scaleGraph.add(1.0f, 0.02f);
                o.alphaGraph = Curve();
                o.alphaGraph.add(0.0f, 0.0f);
                o.alphaGraph.add(0.15f, 1.0f);
                o.alphaGraph.add(1.0f, 0.0f);

                particles->apply();
                particles->play();
            }
        }

        // Alpha-blended puff: a sphere emitter, heavily damped so it billows.
        {
            auto* entity = new Entity();
            entity->setEngine(engine());
            entity->setLocalPosition(1.2f, 0.4f, 0.0f);
            root()->addChild(entity);

            if (auto* particles = static_cast<ParticleSystemComponent*>(
                    entity->addComponent<ParticleSystemComponent>())) {
                auto& o = particles->options();
                o.numParticles = 512;
                o.lifetime = 2.0f;
                o.lifetime2 = 3.0f;
                o.loop = true;
                o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
                o.emitterRadius = 0.25f;
                o.initialVelocity = Vector3(0.0f, 0.5f, 0.0f);
                o.velocitySpread = Vector3(0.35f, 0.35f, 0.35f);
                o.damping = 0.8f;
                o.intensity = 1.0f;
                o.blendType = ParticleBlendType::BLEND_NORMAL;

                o.scaleGraph = Curve();
                o.scaleGraph.add(0.0f, 0.08f);
                o.scaleGraph.add(1.0f, 0.45f);
                o.alphaGraph = Curve();
                o.alphaGraph.add(0.0f, 0.0f);
                o.alphaGraph.add(0.2f, 0.5f);
                o.alphaGraph.add(1.0f, 0.0f);
                // Warm at birth, cooling to blue as it disperses.
                o.colorGraph.curves.resize(3);
                o.colorGraph.curves[0] = Curve();
                o.colorGraph.curves[0].add(0.0f, 1.0f);
                o.colorGraph.curves[0].add(1.0f, 0.25f);
                o.colorGraph.curves[1] = Curve();
                o.colorGraph.curves[1].add(0.0f, 0.6f);
                o.colorGraph.curves[1].add(1.0f, 0.45f);
                o.colorGraph.curves[2] = Curve();
                o.colorGraph.curves[2].add(0.0f, 0.2f);
                o.colorGraph.curves[2].add(1.0f, 1.0f);

                particles->apply();
                particles->play();
            }
        }

        auto* camera = createCamera(Vector3(0.0f, 1.4f, 4.5f));
        addOrbitControls(camera, Vector3(0.0f, 1.0f, 0.0f));

        spdlog::info("Particle system: additive sprite fountain (left) and "
                     "alpha-blended puff (right), both GPU-simulated.");
        return true;
    }

private:
    std::unique_ptr<Asset> _sprite;
};

VISUTWIN_EXAMPLE_MAIN(ParticleSystemExample)
