// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Port of upstream graphics/particles-mesh.
//
// 150 opaque mesh particles — tori, textured with the clouds image through the mesh's own
// UVs and lit by half-Lambert light-cube lighting — burst from a unit sphere above a
// metallic box floor. Each turns to its direction of motion, flies out at a random local
// velocity between +/-8, is pulled up by a world velocity graph that then turns into
// gravity, and changes colour from red through green to blue over its one-second life.
// The helipad sky lights the floor, a white directional light the particles.
//
// Keys stand in for upstream's control panel:
//   L lighting   M align to motion   T textured   E enabled   [ ] lifetime -/+   - = count -/+
//
// DEVIATION: upstream loads its torus from torus.glb, which ships with no licence; the
// port generates the same torus (ring radius 1, tube radius 0.25) with the engine's torus
// primitive geometry.
//
#include <algorithm>
#include <memory>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class ParticlesMeshExample final: public ExampleApp
{
public:
    ParticlesMeshExample(): ExampleApp({.title = "Particles: Mesh"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        // Skydome
        scene()->setSkyboxIntensity(0.5f);
        scene()->setSkyboxMip(2);
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (const auto resource = _helipad->resource(); resource && std::holds_alternative<Texture*>(*resource)) {
            scene()->setEnvAtlas(std::get<Texture*>(*resource));
        }
        _clouds = std::make_unique<Asset>("color", AssetType::TEXTURE, assetPath("textures/clouds.jpg"));
        if (const auto resource = _clouds->resource(); resource && std::holds_alternative<Texture*>(*resource)) {
            _cloudsTexture = std::get<Texture*>(*resource);
        }

        // Camera orbiting (0, 5, 0) from (0, 4, 20).
        auto* camera = createCamera(Vector3(0.0f, 4.0f, 20.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.0f, 0.0f, 0.05f, 1.0f));
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 5.0f, 0.0f))) {
            controls->setZoomRange(Vector2(0.0f, 50.0f));
            controls->storeResetState();
        }

        // The ground
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setGloss(0.6f);
        _groundMaterial->setMetalness(0.4f);
        _groundMaterial->setUseMetalness(true);
        auto* ground = new Entity();
        ground->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>())) {
            render->setMaterial(_groundMaterial.get());
            render->setType("box");
        }
        ground->setLocalScale(10.0f, 1.0f, 10.0f);
        ground->setLocalPosition(0.0f, -0.5f, 0.0f);
        root()->addChild(ground);

        createDirectionalLight(Vector3(25.0f, 0.0f, -80.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, false);

        // The particle mesh: a generated torus (see the DEVIATION above).
        _torus = createMeshFromGeometry(device(), createTorusGeometry(0.25f, 1.0f, 360.0f, 30, 20));

        auto* entity = new Entity();
        entity->setName("Emitter");
        entity->setEngine(engine());
        entity->setLocalPosition(0.0f, 1.0f, 0.0f);
        root()->addChild(entity);
        _particles = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());

        auto& o = _particles->options();
        o.numParticles = 150;
        o.lifetime = o.lifetime2 = 1.0f;
        o.rate = 0.01f;
        o.scaleGraph = Curve({0.0f, 0.2f, 1.0f, 0.7f});
        // Increasing gravity
        o.velocityGraph = CurveSet({{0.0f, 0.0f}, {0.0f, 0.0f, 0.2f, 12.0f, 1.0f, -2.0f}, {0.0f, 0.0f}});
        // Particles move in different directions
        o.localVelocityGraph = CurveSet({{0.0f, 0.0f, 0.5f, 8.0f}, {0.0f, 0.0f, 0.5f, 8.0f}, {0.0f, 0.0f, 0.5f, 8.0f}});
        o.localVelocityGraph2 = CurveSet({{0.0f, 0.0f, 0.5f, -8.0f}, {0.0f, 0.0f, 0.5f, -8.0f}, {0.0f, 0.0f, 0.5f, -8.0f}});
        // Colour through the lifetime
        o.colorGraph = CurveSet({
            {0.0f, 1.0f, 0.25f, 1.0f, 0.375f, 0.5f, 0.5f, 0.0f},
            {0.0f, 0.0f, 0.125f, 0.25f, 0.25f, 0.5f, 0.375f, 0.75f, 0.5f, 1.0f},
            {0.0f, 0.0f, 1.0f, 0.3f}});
        o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
        o.emitterRadius = 1.0f;
        // Mesh and rendering settings
        o.mesh = _torus;
        o.blendType = ParticleBlendType::BLEND_NONE;
        o.depthWrite = true;
        o.lighting = true;
        o.halfLambert = true;
        o.alignToMotion = true;
        // The texture, applied through the mesh's UVs
        o.colorMap = _cloudsTexture;
        _particles->apply();
        _particles->play();

        spdlog::info("Keys: L lighting, M align to motion, T textured, E enabled, [ ] lifetime, - = count");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_particles) {
            return false;
        }
        auto& o = _particles->options();
        switch (event.key.key) {
        case SDLK_L: o.lighting = !o.lighting; break;
        case SDLK_M: o.alignToMotion = !o.alignToMotion; break;
        case SDLK_T: o.colorMap = o.colorMap ? nullptr : _cloudsTexture; break;
        case SDLK_E: _particles->setEnabled(!_particles->enabled()); return true;
        case SDLK_LEFTBRACKET: o.lifetime = o.lifetime2 = std::max(0.1f, o.lifetime - 0.25f); break;
        case SDLK_RIGHTBRACKET: o.lifetime = o.lifetime2 = std::min(5.0f, o.lifetime + 0.25f); break;
        case SDLK_MINUS: o.numParticles = std::max(1u, o.numParticles - 25u); break;
        case SDLK_EQUALS: o.numParticles = std::min(1000u, o.numParticles + 25u); break;
        default: return false;
        }
        _particles->apply();
        spdlog::info("lighting {} align {} textured {} lifetime {:.2f} count {}", o.lighting, o.alignToMotion,
            o.colorMap != nullptr, o.lifetime, o.numParticles);
        return true;
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _clouds;
    Texture* _cloudsTexture = nullptr;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<Mesh> _torus;
    ParticleSystemComponent* _particles = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(ParticlesMeshExample)
