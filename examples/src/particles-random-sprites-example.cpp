// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/particles-random-sprites.
//
// Two particle systems, each picking a random animation from its sprite sheet for every
// particle. The left one plays the coin sheet (4x6 tiles: four animations of six frames, a coin
// spinning in each), the right one the bonus item sheet (4x2 tiles: seven animations of a single
// frame, one item each). Both emit 32 particles from a sphere of radius 2 every 0.2 s, living 2 s,
// drifting outward at 0.125, growing from 0.1 to 0.5 and fading in and out over their lives. The
// two full sprite sheets are shown for reference as image elements on a screen-space screen,
// the coins to the left of the centre and the bonus items to the right.
//
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "core/math/curve.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class ParticlesRandomSpritesExample final: public ExampleApp
{
public:
    ParticlesRandomSpritesExample(): ExampleApp({.title = "Particles: Random Sprites"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
        registerUi(options);
    }

    bool create() override
    {
        _coinsAsset = std::make_unique<Asset>("particlesCoinsTexture", AssetType::TEXTURE,
            assetPath("textures/particles-coins.png"));
        _bonusAsset = std::make_unique<Asset>("particlesBonusTexture", AssetType::TEXTURE,
            assetPath("textures/particles-bonus.png"));
        Texture* coins = _coinsAsset->resourceAs<Texture>();
        Texture* bonus = _bonusAsset->resourceAs<Texture>();
        if (!coins || !bonus) {
            spdlog::error("particles-random-sprites needs textures/particles-coins.png and particles-bonus.png");
            return false;
        }

        // The camera
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 20.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.23f, 0.5f, 0.75f, 1.0f));
        }

        // A directional light
        createDirectionalLight(Vector3(45.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f);

        // A screen to show the particle systems' textures on
        ScreenComponent* screen = createScreen();
        Entity* screenEntity = screen->entity();

        // The emitters: random animations, four of six frames from the coin sheet and seven of
        // one frame from the bonus item sheet
        createEmitter(Vector3(-3.0f, 3.0f, 0.0f), coins, 4, 6, 4, 6);
        createEmitter(Vector3(3.0f, 3.0f, 0.0f), bonus, 4, 2, 7, 1);

        // The full coin sheet, to the left of the centre
        const Vector4 centre(0.5f, 0.5f, 0.5f, 0.5f);
        createElement(engine(), screenEntity, {.type = ElementType::Image, .texture = coins, .anchor = centre,
            .pivot = Vector2(1.75f, 1.0f), .width = 150.0f, .height = 225.0f});

        // The full bonus item sheet, to the right of the centre
        createElement(engine(), screenEntity, {.type = ElementType::Image, .texture = bonus, .anchor = centre,
            .pivot = Vector2(-0.5f, 1.0f), .width = 200.0f, .height = 100.0f});

        return true;
    }

private:
    // A particle system at `position` playing random animations of `sheet`
    void createEmitter(const Vector3& position, Texture* sheet, const int tilesX, const int tilesY,
        const int numAnimations, const int numFrames)
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        root()->addChild(entity);
        entity->setLocalPosition(position);

        auto* particles = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());
        if (!particles) {
            return;
        }
        auto& o = particles->options();
        o.numParticles = 32;
        o.lifetime = 2.0f;
        o.lifetime2 = 2.0f;
        o.rate = 0.2f;
        o.colorMap = sheet;
        o.initialVelocity = 0.125f;
        o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
        o.emitterRadius = 2.0f;
        o.animTilesX = tilesX;
        o.animTilesY = tilesY;
        o.animSpeed = 4.0f;
        o.autoPlay = true;
        // Make the particles fade in and out
        o.alphaGraph = Curve(std::vector<float>{0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f});
        // Gradually make the particles bigger
        o.scaleGraph = Curve(std::vector<float>{0.0f, 0.1f, 1.0f, 0.5f});
        // The number of animations in the sheet, the frames in each, and a random one per particle
        o.animNumAnimations = numAnimations;
        o.animNumFrames = numFrames;
        o.randomizeAnimIndex = true;

        particles->apply();
        particles->play();
    }

    std::unique_ptr<Asset> _coinsAsset;
    std::unique_ptr<Asset> _bonusAsset;
};

VISUTWIN_EXAMPLE_MAIN(ParticlesRandomSpritesExample)
