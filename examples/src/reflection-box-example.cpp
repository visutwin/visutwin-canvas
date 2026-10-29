// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/reflection-box.
//
// Box-projected reflections from a live probe. A room 800 x 400 x 800 built from boxes (white,
// metalness workflow, the tiling normal map at 5 x 5) holds magenta emissive pillars and beams, a
// TV screen and a large shiny sphere (scale 300 at (0, 150, 0)). A probe at (0, 200, 0) renders the
// World layer into a 128 cube every 10th frame (near 1, far 500), and each capture is prefiltered
// into a 512 environment atlas that both the walls and the sphere reflect as their OWN atlas
// (StandardMaterial::setEnvAtlas), not the scene's. The walls project it onto the room's box
// (CUBEPROJ_BOX, centre (0, 200, 0), half extents (400, 200, 400)), so their reflections line up
// with the room as the view moves; the sphere's does not. A white omni light (intensity 0.2, range
// 1000) and its marker sphere circle the ceiling on an Excluded layer, which the probe does not
// capture. Every frame both materials take the controls' gloss 0.8, metalness 0.9, bumpiness 0.2
// and reflectivity 0.5. The camera (fov 100, far 1500) looks from (270, 90, -260) at
// (0, 200, 1.25), the centre of the walls and the screen, and orbits it with the pitch held to
// +/-90 degrees and the distance capped at 390, which pulls the start in by 1.5 units.
//
// Controls:
//   [ / ]  probe update frequency -/+ 1 frame (0 = every frame)
//   1 / 2  gloss -/+ 0.05      3 / 4  metalness -/+ 0.05
//   5 / 6  reflectivity -/+ 0.05      7 / 8  bumpiness -/+ 0.05
//
// DEVIATIONS:
// - The TV screen shows a still of upstream's video (sample-video-still.jpg, its frame at 2 s):
//   there are no video textures here.
// - The probe is the engine's ReflectionProbe, standing in for upstream's cubemapRenderer script;
//   it also installs itself as the scene's probe, which no material here opts into.
// - The atlas is RGBP, the engine's atlas encoding, where upstream asks for RGBM.
// - There is no cubemap preview (upstream's showCubemap draws the probe's faces with its
//   TextureRenderer).
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/extras/reflectionProbe.h"
#include "platform/input/keyboard.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // The layer of what the probe does not capture.
    constexpr int LAYERID_EXCLUDED = 100;
}

class ReflectionBoxExample final : public ExampleApp
{
public:
    ReflectionBoxExample() : ExampleApp({.title = "Reflection Box"}) {}

protected:
    bool create() override
    {
        _normal = std::make_unique<Asset>("normal", AssetType::TEXTURE, assetPath("textures/normal-map.png"));
        _video = std::make_unique<Asset>("video", AssetType::TEXTURE, assetPath("textures/sample-video-still.jpg"));
        Texture* normal = _normal->resourceAs<Texture>();
        Texture* video = _video->resourceAs<Texture>();
        if (!normal || !video) {
            spdlog::error("reflection-box: a texture failed to load");
            return false;
        }

        // A layer for what does not render into the probe, after the world's transparent sublayer
        const auto layers = scene()->layers();
        const auto worldLayer = layers->getLayerById(LAYERID_WORLD);
        layers->insert(std::make_shared<Layer>("Excluded", LAYERID_EXCLUDED), layers->getTransparentIndex(worldLayer) + 1);

        // The environment atlas the probe's captures are prefiltered into
        TextureOptions atlasOptions;
        atlasOptions.name = "reflection-box-atlas";
        atlasOptions.width = 512;
        atlasOptions.height = 512;
        atlasOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        atlasOptions.mipmaps = false;
        atlasOptions.minFilter = FilterMode::FILTER_LINEAR;
        atlasOptions.magFilter = FilterMode::FILTER_LINEAR;
        atlasOptions.projection = TextureProjection::TEXTUREPROJECTION_EQUIRECT;
        _envAtlas = std::make_shared<Texture>(device().get(), atlasOptions);
        _envAtlas->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _envAtlas->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        _envAtlas->setEncoding(TextureEncoding::RGBP);
        _envAtlas->upload();

        // The walls: the atlas projected onto the room's box
        _roomMaterial = std::make_shared<StandardMaterial>();
        _roomMaterial->setUseMetalness(true);
        _roomMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _roomMaterial->setNormalMap(normal);
        _roomMaterial->setNormalMapTiling(Vector2(5.0f, 5.0f));
        _roomMaterial->setEnvAtlas(_envAtlas.get());
        _roomMaterial->setCubeMapProjection(CUBEPROJ_BOX);
        _roomMaterial->setCubeMapProjectionBox(
            BoundingBox(Vector3(0.0f, 200.0f, 0.0f), Vector3(400.0f, 200.0f, 400.0f)));

        // The magenta beams, and the marker of the light
        _emissiveMaterial = std::make_shared<StandardMaterial>();
        _emissiveMaterial->setEmissive(Color(1.0f, 0.0f, 1.0f, 1.0f));
        _emissiveMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _lightMaterial = std::make_shared<StandardMaterial>();
        _lightMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _lightMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));

        // The sphere: the same atlas, not projected
        _sphereMaterial = std::make_shared<StandardMaterial>();
        _sphereMaterial->setUseMetalness(true);
        _sphereMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _sphereMaterial->setNormalMap(normal);
        _sphereMaterial->setNormalMapTiling(Vector2(5.0f, 5.0f));
        _sphereMaterial->setEnvAtlas(_envAtlas.get());

        // The TV screen
        _screenMaterial = std::make_shared<StandardMaterial>();
        _screenMaterial->setUseLighting(false);
        _screenMaterial->setEmissiveMap(video);
        _screenMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));

        // The probe, constructed before the main camera so its faces render first
        _probe = std::make_unique<ReflectionProbe>(engine(), 128);
        _probe->setPosition(Vector3(0.0f, 200.0f, 0.0f));
        _probe->setLayers({LAYERID_WORLD});
        _probe->setNearFar(1.0f, 500.0f);
        _probe->setToneMapping(TONEMAP_LINEAR);
        _probe->setDynamic(false);

        // The ground plane and the ceiling
        addBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(800.0f, 2.0f, 800.0f), _roomMaterial.get());
        addBox(Vector3(0.0f, 400.0f, 0.0f), Vector3(800.0f, 2.0f, 800.0f), _roomMaterial.get());
        // Walls
        addBox(Vector3(400.0f, 200.0f, 0.0f), Vector3(2.0f, 400.0f, 800.0f), _roomMaterial.get());
        addBox(Vector3(-400.0f, 200.0f, 0.0f), Vector3(2.0f, 400.0f, 800.0f), _roomMaterial.get());
        addBox(Vector3(0.0f, 200.0f, -400.0f), Vector3(800.0f, 400.0f, 0.0f), _roomMaterial.get());
        addBox(Vector3(0.0f, 200.0f, 400.0f), Vector3(800.0f, 400.0f, 0.0f), _roomMaterial.get());
        // Emissive pillars
        addBox(Vector3(400.0f, 200.0f, -50.0f), Vector3(20.0f, 400.0f, 20.0f), _emissiveMaterial.get());
        addBox(Vector3(400.0f, 200.0f, 50.0f), Vector3(20.0f, 400.0f, 20.0f), _emissiveMaterial.get());
        addBox(Vector3(-400.0f, 200.0f, 50.0f), Vector3(20.0f, 400.0f, 20.0f), _emissiveMaterial.get());
        addBox(Vector3(-400.0f, 200.0f, -50.0f), Vector3(20.0f, 400.0f, 20.0f), _emissiveMaterial.get());
        addBox(Vector3(0.0f, 400.0f, 50.0f), Vector3(800.0f, 20.0f, 20.0f), _emissiveMaterial.get());
        addBox(Vector3(0.0f, 400.0f, -50.0f), Vector3(800.0f, 20.0f, 20.0f), _emissiveMaterial.get());
        // Screen
        addBox(Vector3(0.0f, 200.0f, 400.0f), Vector3(500.0f, 250.0f, 5.0f), _screenMaterial.get());

        // The shiny sphere, on the World layer only
        Entity* sphere = createPrimitive("sphere", _sphereMaterial.get(), Vector3(0.0f, 150.0f, 0.0f),
            Vector3(300.0f, 300.0f, 300.0f), {LAYERID_WORLD});
        noShadows(sphere);

        // The orbiting omni light, with a marker, both kept out of the probe
        _light = createPrimitive("sphere", _lightMaterial.get(), Vector3(0.0f, 300.0f, 300.0f),
            Vector3(20.0f, 20.0f, 20.0f), {LAYERID_EXCLUDED});
        noShadows(_light);
        auto* light = static_cast<LightComponent*>(_light->addComponent<LightComponent>());
        light->setType(LightType::LIGHTTYPE_OMNI);
        light->setLayers({LAYERID_EXCLUDED});
        light->setCastShadows(false);
        light->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        light->setIntensity(0.2f);
        light->setRange(1000.0f);

        // The camera, looking at the room's centre from at most 390 units (see the header)
        const Vector3 pivot(0.0f, 200.0f, 1.25f);
        const Vector3 authored(270.0f, 90.0f, -260.0f);
        const float distance = std::min((authored - pivot).length(), 390.0f);
        Entity* camera = createCamera(pivot + (authored - pivot).normalized() * distance);
        camera->setName("MainCamera");
        camera->lookAt(pivot);
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_EXCLUDED, LAYERID_UI});
        cameraComponent->camera()->setFov(100.0f);
        cameraComponent->camera()->setFarClip(1500.0f);
        if (CameraControls* controls = addOrbitControls(camera, pivot)) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.01f, 390.0f));
            controls->storeResetState();
        }

        applySettings();
        spdlog::info("Keys: [ ] probe frequency, 1/2 gloss, 3/4 metalness, 5/6 reflectivity, 7/8 bumpiness");
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * 0.3f;
        handleKeys();

        // The light circles the ceiling
        _light->setLocalPosition(300.0f * std::sin(_time), 300.0f, 300.0f * std::cos(_time));

        // The probe renders every updateFrequency-th frame (every frame at 0)
        --_updateProbeCount;
        if (_updateFrequency == 0) {
            _updateProbeCount = 1;
        }
        if (_updateProbeCount <= 0) {
            _probe->requestBake();
            _captureThisFrame = true;
            _updateProbeCount = _updateFrequency;
        }
        applySettings();
    }

    void postRender() override
    {
        // A capture just rendered: build its mips, then prefilter it into the atlas both
        // materials reflect for the frames until the next one. The captured cube holds
        // gamma-encoded colour, which the bake decodes.
        _probe->update();
        if (_captureThisFrame) {
            _captureThisFrame = false;
            EnvLighting::generateAtlas(device().get(), _probe->cubemap(), _envAtlas.get(), 512, 1024, 2048, true);
        }
    }

    void destroy() override
    {
        for (const auto& material : {_roomMaterial, _sphereMaterial}) {
            if (material) {
                material->setEnvAtlas(nullptr);
            }
        }
        _probe.reset();
    }

private:
    void addBox(const Vector3& position, const Vector3& scale, Material* material)
    {
        Entity* box = createPrimitive("box", material, position, scale, {LAYERID_WORLD, LAYERID_EXCLUDED});
        noShadows(box);
    }

    static void noShadows(Entity* entity)
    {
        if (auto* render = entity->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }
    }

    void handleKeys()
    {
        auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        const auto step = [&](const Key down, const Key up, float& value, const float low, const float high) {
            if (keyboard->wasPressed(down)) value = std::max(low, value - 0.05f);
            if (keyboard->wasPressed(up)) value = std::min(high, value + 0.05f);
        };
        step(Key::Digit1, Key::Digit2, _gloss, 0.0f, 1.0f);
        step(Key::Digit3, Key::Digit4, _metalness, 0.0f, 1.0f);
        step(Key::Digit5, Key::Digit6, _reflectivity, 0.0f, 1.0f);
        step(Key::Digit7, Key::Digit8, _bumpiness, 0.0f, 1.0f);
        if (keyboard->wasPressed(Key::LeftBracket)) _updateFrequency = std::max(0, _updateFrequency - 1);
        if (keyboard->wasPressed(Key::RightBracket)) _updateFrequency = std::min(10, _updateFrequency + 1);
    }

    // Both reflective materials take the controls' values every frame.
    void applySettings()
    {
        for (const auto& material : {_roomMaterial, _sphereMaterial}) {
            material->setGloss(_gloss);
            material->setMetalness(_metalness);
            material->setBumpiness(_bumpiness);
            material->setReflectivity(_reflectivity);
        }
    }

    std::unique_ptr<Asset> _normal;
    std::unique_ptr<Asset> _video;
    std::shared_ptr<Texture> _envAtlas;
    std::shared_ptr<StandardMaterial> _roomMaterial;
    std::shared_ptr<StandardMaterial> _emissiveMaterial;
    std::shared_ptr<StandardMaterial> _lightMaterial;
    std::shared_ptr<StandardMaterial> _sphereMaterial;
    std::shared_ptr<StandardMaterial> _screenMaterial;
    std::unique_ptr<ReflectionProbe> _probe;
    Entity* _light = nullptr;
    float _time = 0.0f;
    int _updateFrequency = 10;
    int _updateProbeCount = 1;
    bool _captureThisFrame = false;
    float _gloss = 0.8f;
    float _metalness = 0.9f;
    float _bumpiness = 0.2f;
    float _reflectivity = 0.5f;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionBoxExample)
