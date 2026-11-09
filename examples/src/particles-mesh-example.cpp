// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// Port of upstream particles/mesh.
//
// A fountain of 150 opaque mesh particles — tori, textured through the mesh's own UVs with a
// procedural diagonal-stripe texture and lit by half-Lambert light-cube lighting — arcs out
// of a glowing pedestal on a dark stage. Each ring turns to its direction of motion, shoots
// up at a random local velocity (8-10 up, +/-1.4 sideways), is pulled back down by a linear
// gravity graph, pops in, holds its size and shrinks away over its 2.5-second life, changing
// colour from gold through pink to violet. The emitter starts pre-warmed, so the fountain is
// full from the first frame.
//
// The stage: a glossy dark floor fading into the black background through linear fog, a
// metallic pedestal with a thin emissive ring around its top, a warm omni light spilling the
// ring's glow onto the floor, a warm key and a cool rim directional light (which, with the
// ambient light, shade the particles), and the studio environment atlas for the reflections
// (the camera does not render the skybox layer, so the sky is not visible). The camera renders
// through a camera frame: 4x MSAA, neutral tone mapping, bloom and a vignette.
//
// Keys stand in for upstream's control panel:
//   L lighting   M align to motion   T textured   E enabled   [ ] lifetime -/+   - = count -/+
//
// Each particle is meant to pick a random colour between colorGraph and colorGraph2; neither
// upstream's GPU particle path nor this engine samples colorGraph2, so every ring follows
// colorGraph. The second graph is set anyway, so the authoring carries over unchanged.
//
// DEVIATION: upstream loads its torus from torus.glb, which ships with no licence; the
// port generates the same torus (ring radius 1, tube radius 0.25) with the engine's torus
// primitive geometry.
// DEVIATION: upstream renders at up to two pixels per point; the example harness renders at
// one, as every example here does.
//
#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

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
        // A dark stage: the floor fades into the black background, so it has no visible edge
        const Color backgroundColor(0.0f, 0.0f, 0.0f, 1.0f);
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_LINEAR);
        scene()->setFogColor(backgroundColor);
        scene()->setFogLinear(10.0f, 28.0f);

        // The environment gives the glossy floor and pedestal their reflections. The camera
        // does not render the skybox layer, so it is not visible.
        _envAtlas = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (Texture* envAtlas = _envAtlas->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setSkyboxIntensity(0.4f);

        // Mesh particles are not lit by the environment, only by the ambient light and the
        // directional lights
        scene()->setAmbientLight(0.16f, 0.12f, 0.22f);

        createCameraAndFrame(backgroundColor);
        createStage();
        createLights();
        createStripesTexture();
        createEmitter();

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
        // Switching the colour map off falls back to white, showing the mesh UVs do the texturing
        case SDLK_T: o.colorMap = o.colorMap ? nullptr : _stripes.get(); break;
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
    void createCameraAndFrame(const Color& backgroundColor)
    {
        auto* camera = createCamera(Vector3(0.0f, 3.5f, 13.5f));
        camera->setName("Camera");
        auto* component = camera->findComponent<CameraComponent>();
        if (component) {
            if (component->camera()) {
                component->camera()->setClearColor(backgroundColor);
            }
            component->setLayers({LAYERID_WORLD, LAYERID_UI});

            // The neutral tone mapping keeps the particle colours, and the bloom makes the
            // highlights glow
            component->setToneMapping(TONEMAP_NEUTRAL);
            auto& rendering = component->rendering();
            rendering.samples = 4;
            rendering.bloomIntensity = 0.02f;
            rendering.bloomBlurLevel = 5;
            rendering.vignetteEnabled = true;
            rendering.vignetteIntensity = 0.5f;
            rendering.vignetteInner = 0.4f;
            rendering.vignetteOuter = 1.3f;
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 3.6f, 0.0f))) {
            controls->setPitchRange(Vector2(-90.0f, 5.0f));
            controls->setZoomRange(Vector2(4.0f, 30.0f));
            controls->storeResetState();
        }
    }

    static std::shared_ptr<StandardMaterial> createMaterial(const Color& color, const float gloss,
        const float metalness)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setGloss(gloss);
        material->setMetalness(metalness);
        material->setUseMetalness(true);
        return material;
    }

    void createStage()
    {
        // A large glossy floor
        _floorMaterial = createMaterial(Color(0.06f, 0.05f, 0.09f), 0.6f, 0.0f);
        createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(80.0f, 1.0f, 80.0f))
            ->setName("Floor");

        // The pedestal the particles erupt from, with a thin glowing ring around its top
        _pedestalMaterial = createMaterial(Color(0.1f, 0.09f, 0.12f), 0.85f, 0.6f);
        createPrimitive("cylinder", _pedestalMaterial.get(), Vector3(0.0f, 0.4f, 0.0f), Vector3(3.0f, 0.8f, 3.0f))
            ->setName("Pedestal");

        _ringMaterial = std::make_shared<StandardMaterial>();
        _ringMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f));
        _ringMaterial->setEmissive(Color(1.0f, 0.55f, 0.3f));
        _ringMaterial->setEmissiveIntensity(8.0f);

        // Ring radius 1.5, tube radius 0.04, 64 segments (and the torus geometry's default 20 sides)
        _ringMesh = createMeshFromGeometry(device(), createTorusGeometry(0.04f, 1.5f, 360.0f, 64, 20));
        auto* ring = new Entity();
        ring->setEngine(engine());
        ring->setName("Ring");
        if (auto* render = static_cast<RenderComponent*>(ring->addComponent<RenderComponent>())) {
            render->addMeshInstance(std::make_unique<MeshInstance>(_ringMesh, _ringMaterial, ring));
        }
        ring->setLocalPosition(0.0f, 0.8f, 0.0f);
        root()->addChild(ring);
    }

    void createLights()
    {
        // A warm light above the pedestal, spilling its glow onto the floor
        auto* glow = new Entity();
        glow->setEngine(engine());
        glow->setName("Glow");
        if (auto* light = static_cast<LightComponent*>(glow->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_OMNI);
            light->setColor(Color(1.0f, 0.55f, 0.3f));
            light->setIntensity(3.0f);
            light->setRange(8.0f);
        }
        glow->setLocalPosition(0.0f, 1.5f, 0.0f);
        root()->addChild(glow);

        // A warm key light from the front left, and a cool rim light from behind. These two, and
        // the ambient light, shade the particles.
        createDirectionalLight(Vector3(45.0f, -30.0f, 30.0f), Color(1.0f, 0.85f, 0.7f), 1.4f, false)
            ->setName("Key Light");
        createDirectionalLight(Vector3(-60.0f, 150.0f, 0.0f), Color(0.45f, 0.55f, 1.0f), 1.2f, false)
            ->setName("Rim Light");
    }

    void createStripesTexture()
    {
        // A procedural texture with diagonal stripes, which shows how the particles are textured
        // using the mesh UVs. The particle colours tint it. Its bytes are gamma encoded; the
        // particle program decodes the colour map.
        constexpr uint32_t size = 64;
        TextureOptions options;
        options.name = "Stripes";
        options.width = size;
        options.height = size;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        _stripes = std::make_shared<Texture>(device().get(), options);

        std::vector<uint8_t> pixels(size * size * 4);
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const auto value = static_cast<uint8_t>((((x + y) >> 3) & 1u) != 0 ? 255 : 120);
                const size_t i = (y * size + x) * 4;
                pixels[i] = pixels[i + 1] = pixels[i + 2] = value;
                pixels[i + 3] = 255;
            }
        }
        _stripes->setLevelData(0, pixels.data(), pixels.size());
        _stripes->upload();
    }

    void createEmitter()
    {
        // The particle mesh: a generated torus (see the DEVIATION above).
        _torus = createMeshFromGeometry(device(), createTorusGeometry(0.25f, 1.0f, 360.0f, 30, 20));

        // The emitter, at the top of the pedestal
        auto* entity = new Entity();
        entity->setName("Emitter");
        entity->setEngine(engine());
        entity->setLocalPosition(0.0f, 1.0f, 0.0f);
        root()->addChild(entity);
        _particles = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());

        auto& o = _particles->options();
        o.numParticles = 150;
        o.lifetime = o.lifetime2 = 2.5f;
        o.rate = 0.016f;
        o.preWarm = true;

        // Particles pop in, hold their size, and shrink away as they fall back down
        o.scaleGraph = Curve({0.0f, 0.0f, 0.08f, 0.3f, 0.8f, 0.28f, 1.0f, 0.0f});
        o.scaleGraph2 = Curve({0.0f, 0.0f, 0.08f, 0.2f, 0.8f, 0.18f, 1.0f, 0.0f});

        // Particles shoot upwards with a random sideways spread ...
        o.localVelocityGraph = CurveSet({{0.0f, -1.4f}, {0.0f, 8.0f}, {0.0f, -1.4f}});
        o.localVelocityGraph2 = CurveSet({{0.0f, 1.4f}, {0.0f, 10.0f}, {0.0f, 1.4f}});

        // ... and gravity pulls them back down. The downward velocity grows linearly, so they
        // fly in arcs.
        o.velocityGraph = CurveSet({{0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, -18.0f}, {0.0f, 0.0f}});
        o.velocityGraph.setType(CURVE_LINEAR);

        // Colour from gold to pink to violet over the lifetime (colorGraph2: see the header)
        o.colorGraph = CurveSet({
            {0.0f, 1.0f, 0.5f, 1.0f, 1.0f, 0.6f},
            {0.0f, 0.75f, 0.5f, 0.35f, 1.0f, 0.3f},
            {0.0f, 0.3f, 0.5f, 0.55f, 1.0f, 1.0f}});
        o.colorGraph2 = CurveSet({
            {0.0f, 1.0f, 0.5f, 0.9f, 1.0f, 0.35f},
            {0.0f, 0.55f, 0.5f, 0.2f, 1.0f, 0.45f},
            {0.0f, 0.15f, 0.5f, 0.7f, 1.0f, 1.0f}});
        o.intensity = 1.2f;

        o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
        o.emitterRadius = 0.5f;

        // Mesh and rendering settings
        o.mesh = _torus;
        o.blendType = ParticleBlendType::BLEND_NONE;
        o.depthWrite = true;
        o.lighting = true;
        o.halfLambert = true;
        o.alignToMotion = true;

        // The texture, applied to the mesh particles through the mesh UVs
        o.colorMap = _stripes.get();
        _particles->apply();
        _particles->play();
    }

    std::unique_ptr<Asset> _envAtlas;
    std::shared_ptr<Texture> _stripes;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::shared_ptr<StandardMaterial> _pedestalMaterial;
    std::shared_ptr<StandardMaterial> _ringMaterial;
    std::shared_ptr<Mesh> _ringMesh;
    std::shared_ptr<Mesh> _torus;
    ParticleSystemComponent* _particles = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(ParticlesMeshExample)
