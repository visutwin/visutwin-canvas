// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/particles-anim-index.
//
// Four particle systems at the corners of the view, all sharing one 4x4 sprite
// sheet of numbered tiles. Each declares four frames per animation and a different
// `animIndex`, so the four play the four rows of the sheet: 1-2-3-4, 5-6-7-8, and
// so on. It is the demonstration of animIndex, which is why every other setting is
// identical between them.
//
// This is also the first example of ParticleSystemComponent since the campfire
// scene was replaced, and a component system only ticks if the APPLICATION
// registers it, hence the registerComponentSystem call in configure().
//
// DEVIATION: upstream also draws the whole sprite sheet on a screen-space UI panel
// for reference. That is left out here.
//
#include <memory>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class ParticlesAnimIndexExample final: public ExampleApp
{
public:
    ParticlesAnimIndexExample(): ExampleApp({.title = "Particles: Animation Index"}) {}

protected:
    void configure(AppOptions& options) override
    {
        // Without this the component is inert: nothing else ticks it.
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        _sprite = std::make_unique<Asset>("particlesNumbers", AssetType::TEXTURE,
            assetPath("textures/particles-numbers.png"));
        Texture* sheet = nullptr;
        if (const auto resource = _sprite->resource();
            resource && std::holds_alternative<Texture*>(*resource)) {
            sheet = std::get<Texture*>(*resource);
        }
        if (sheet == nullptr) {
            spdlog::error("particles-anim-index needs textures/particles-numbers.png");
            return false;
        }

        createDirectionalLight(Vector3(45.0f, 0.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, false);

        const Vector3 positions[] = {
            Vector3(-3.0f, 3.0f, 0.0f), Vector3(3.0f, 3.0f, 0.0f),
            Vector3(-3.0f, -3.0f, 0.0f), Vector3(3.0f, -3.0f, 0.0f),
        };

        for (int i = 0; i < 4; ++i) {
            auto* entity = new Entity();
            entity->setEngine(engine());
            entity->setLocalPosition(positions[i]);
            root()->addChild(entity);

            auto* particles = static_cast<ParticleSystemComponent*>(
                entity->addComponent<ParticleSystemComponent>());
            if (particles == nullptr) {
                continue;
            }
            auto& o = particles->options();
            o.numParticles = 8;
            o.lifetime = 4.0f;
            o.lifetime2 = 4.0f;
            o.rate = 0.5f;
            o.loop = true;
            o.colorMap = sheet;
            o.initialVelocity = Vector3(0.0f, 0.25f, 0.0f);
            o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
            o.emitterRadius = 0.1f;
            o.animTilesX = 4;
            o.animTilesY = 4;
            o.animSpeed = 1.0f;
            // Four frames per animation, and one animation each.
            o.animNumFrames = 4;
            o.animIndex = i;
            // Gradually make the sprites bigger over their life.
            o.scaleGraph = Curve();
            o.scaleGraph.add(0.0f, 0.0f);
            o.scaleGraph.add(1.0f, 1.0f);

            particles->apply();
            particles->play();
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 20.0f));
        addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f));

        spdlog::info("Four emitters, one sprite sheet, animIndex 0 to 3.");
        return true;
    }

private:
    std::unique_ptr<Asset> _sprite;
};

VISUTWIN_EXAMPLE_MAIN(ParticlesAnimIndexExample)
