// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/reflection-box.
//
// A reflection probe renders the room (area-lights-room.glb) into a cubemap at runtime, every 10th
// frame, prefiltered into a 512 environment atlas that the room, the statue and the objects in it
// reflect and are lit by, each as its OWN atlas (StandardMaterial::setEnvAtlas). Box projection
// maps the reflections onto the inside of the room (centre (0, 3.25, 0), half extents (8, 3.25, 9),
// where the probe renders from), so they line up with what they reflect. The probe renders in HDR,
// so the reflections keep the brightness of the glowing doors, the screen above the middle door and
// a drifting orb light. A bumpy chrome sphere and a spinning gold torus are on a layer the probe
// does not render, so they are not in the reflections. Turned off, the reflections leave the
// materials a small constant ambient light. Three door spot lights and the orb's omni light cast
// shadows; the camera frame (4x MSAA, ACES) adds bloom and a vignette.
//
// Keys stand in for upstream's controls panel:
//   R            reflections on or off
//   [ / ]        probe update frequency -/+ 1 frame (0: once)
//   P            box projection on or off
//   5 / 6        reflectivity -/+ 0.05
//   - / =        the floor's roughness -/+ 0.05      9 / 0   the floor's metalness -/+ 0.05
//   1 / 2        the sphere's roughness -/+ 0.05     3 / 4   its bumpiness -/+ 0.05
//   M            the sphere metallic or not
//   Space        pause the animation
//
// DEVIATIONS:
// - The screen shows a still of upstream's video (sample-video-still.jpg, its frame at 2 s): there
//   are no video textures here.
// - The probe is the engine's ReflectionProbe (HDR), standing in for upstream's cubemapRenderer
//   script; it also installs itself as the scene's probe, read as linear.
// - The atlas is RGBP, the engine's atlas encoding, where upstream asks for RGBM.
// - There is no cubemap preview (upstream's showCubemap draws the probe's faces).
// - The glTF materials are found from the instantiated meshes rather than the container's list.
// - Our glTF materials keep gloss as 1 - roughness (upstream's loader sets glossInvert and stores the
//   roughness), so the floor's roughness is written as 1 - roughness.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/extras/reflectionProbe.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The layer of what the probe does not render.
    constexpr int LAYERID_EXCLUDED = 100;

    // The sky behind the skylight and the windows.
    const Color kSkyColor(0.45f, 0.6f, 0.8f, 1.0f);
}

class ReflectionBoxExample final : public ExampleApp
{
public:
    ReflectionBoxExample() : ExampleApp({}) {}

protected:
    bool create() override
    {
        _room = std::make_unique<Asset>("room", AssetType::CONTAINER, assetPath("models/area-lights-room.glb"));
        _statue = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _normal = std::make_unique<Asset>("normal", AssetType::TEXTURE, assetPath("textures/normal-map.png"));
        _video = std::make_unique<Asset>("video", AssetType::TEXTURE, assetPath("textures/sample-video-still.jpg"));
        auto* roomContainer = _room->resourceAs<ContainerResource>();
        auto* statueContainer = _statue->resourceAs<ContainerResource>();
        Texture* normal = _normal->resourceAs<Texture>();
        Texture* video = _video->resourceAs<Texture>();
        if (!roomContainer || !statueContainer || !normal || !video) {
            spdlog::error("reflection-box: an asset did not load");
            return false;
        }

        auto& lighting = scene()->lighting();
        lighting.shadowsEnabled = true;
        lighting.shadowAtlasResolution = 2048;

        // A layer for what the probe does not render, lit by the lights on both layers.
        const auto layers = scene()->layers();
        const auto worldLayer = layers->getLayerById(LAYERID_WORLD);
        layers->insert(std::make_shared<Layer>("Excluded", LAYERID_EXCLUDED), layers->getTransparentIndex(worldLayer) + 1);
        const std::vector<int> lightLayers = {LAYERID_WORLD, LAYERID_EXCLUDED};

        // The environment atlas each capture is prefiltered into.
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

        // The probe, constructed before the main camera so its faces render first: the World
        // layer from the centre of the room's box, in HDR.
        _probe = std::make_unique<ReflectionProbe>(engine(), 256, true);
        _probe->setPosition(Vector3(0.0f, 3.25f, 0.0f));
        _probe->setLayers({LAYERID_WORLD});
        _probe->setNearFar(0.1f, 30.0f);
        _probe->setClearColor(kSkyColor);
        _probe->setDynamic(false);

        // The room and the statue, both reflecting the room.
        Entity* room = roomContainer->instantiateRenderEntity();
        room->setEngine(engine());
        root()->addChild(room);
        Entity* statue = statueContainer->instantiateRenderEntity();
        statue->setEngine(engine());
        statue->setLocalScale(0.12f, 0.12f, 0.12f);
        statue->setLocalPosition(-4.0f, 0.55f, 0.8f);
        statue->setLocalEulerAngles(0.0f, 25.0f, 0.0f);
        root()->addChild(statue);
        for (Entity* model : {room, statue}) {
            for (auto* render : model->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
                for (auto* meshInstance : render->meshInstances()) {
                    auto* material = dynamic_cast<StandardMaterial*>(meshInstance ? meshInstance->material() : nullptr);
                    if (material && std::find(_reflective.begin(), _reflective.end(), material) == _reflective.end()) {
                        _reflective.push_back(material);
                        if (material->name() == "Floor") {
                            _floorMaterial = material;
                        }
                    }
                }
            }
        }
        if (!_floorMaterial) {
            spdlog::error("reflection-box: the room has no 'Floor' material");
            return false;
        }

        // The bumpy chrome sphere on the centre pedestal and a gold torus spinning on the right
        // one, both kept out of the probe.
        _sphereMaterial = createReflectiveMaterial(Color(0.95f, 0.95f, 0.95f, 1.0f));
        _sphereMaterial->setNormalMap(normal);
        _sphereMaterial->setNormalMapTiling(Vector2(5.0f, 5.0f));
        createPrimitive("sphere", _sphereMaterial.get(), Vector3(0.0f, 1.26f, 0.0f), Vector3(1.8f, 1.8f, 1.8f),
            {LAYERID_EXCLUDED});

        _torusMaterial = createReflectiveMaterial(Color(1.0f, 0.77f, 0.34f, 1.0f));
        _torusMaterial->setGloss(0.25f);
        _torusPivot = new Entity();
        _torusPivot->setName("Torus");
        _torusPivot->setEngine(engine());
        _torusPivot->setLocalPosition(4.0f, 1.29f, 0.8f);
        root()->addChild(_torusPivot);
        Entity* torus = createPrimitive("torus", _torusMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(1.5f, 1.5f, 1.5f), {LAYERID_EXCLUDED});
        _torusPivot->addChild(torus);
        torus->setLocalPosition(0.0f, 0.0f, 0.0f);
        torus->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

        // The glowing doors, each with a shadowed spot light shining into the room (down its -Y
        // axis, turned to face the room).
        const struct
        {
            float x;
            Color color;
        } doors[] = {
            {-4.5f, Color(1.0f, 0.42f, 0.12f, 1.0f)},
            {0.0f, Color(0.25f, 0.8f, 1.0f, 1.0f)},
            {4.5f, Color(1.0f, 0.8f, 0.35f, 1.0f)},
        };
        for (const auto& door : doors) {
            createPrimitive("box", createEmissiveMaterial(door.color, 4.0f).get(), Vector3(door.x, 1.7f, -9.45f),
                Vector3(1.7f, 3.3f, 0.02f));
            auto* light = new Entity();
            light->setEngine(engine());
            if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
                lc->setType(LightType::LIGHTTYPE_SPOT);
                lc->setLayers(lightLayers);
                lc->setColor(door.color);
                lc->setIntensity(1.5f);
                lc->setRange(20.0f);
                lc->setInnerConeAngle(50.0f);
                lc->setOuterConeAngle(80.0f);
                lc->setCastShadows(true);
                lc->setShadowBias(0.2f);
                lc->setShadowNormalBias(0.05f);
            }
            light->setLocalPosition(door.x, 1.7f, -9.2f);
            light->setLocalEulerAngles(-90.0f, 0.0f, 0.0f);
            root()->addChild(light);
        }

        // The screen above the middle door, in a dark frame.
        auto screenMaterial = createEmissiveMaterial(Color(1.0f, 1.0f, 1.0f, 1.0f), 1.2f);
        screenMaterial->setEmissiveMap(video);
        Entity* screen = createPrimitive("plane", screenMaterial.get(), Vector3(0.0f, 4.75f, -8.58f),
            Vector3(3.4f, 1.0f, 1.91f));
        screen->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        auto frameMaterial = std::make_shared<StandardMaterial>();
        frameMaterial->setDiffuse(Color(0.02f, 0.02f, 0.025f, 1.0f));
        _materials.push_back(frameMaterial);
        createPrimitive("box", frameMaterial.get(), Vector3(0.0f, 4.75f, -8.62f), Vector3(3.5f, 2.01f, 0.06f));

        // A glowing orb drifting around the room, which the probe renders into the reflections.
        _orb = createPrimitive("sphere", createEmissiveMaterial(Color(0.8f, 0.88f, 1.0f, 1.0f), 6.0f).get(),
            Vector3(0.0f, 2.2f, 3.7f), Vector3(0.4f, 0.4f, 0.4f));
        _orb->setName("Orb");
        if (auto* render = _orb->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }
        if (auto* lc = static_cast<LightComponent*>(_orb->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_OMNI);
            lc->setLayers(lightLayers);
            lc->setColor(Color(0.8f, 0.88f, 1.0f, 1.0f));
            lc->setIntensity(1.5f);
            lc->setRange(10.0f);
            lc->setCastShadows(true);
            lc->setShadowBias(0.2f);
            lc->setShadowNormalBias(0.05f);
        }

        // With the reflections off, a small constant ambient light, tinted by the sky.
        scene()->setAmbientLight(0.04f, 0.045f, 0.055f);

        // The camera at the front of the room, looking at the doors.
        Entity* camera = createCamera(Vector3(1.3f, 2.5f, 6.8f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_EXCLUDED, LAYERID_UI});
        cameraComponent->camera()->setClearColor(kSkyColor);
        cameraComponent->camera()->setFov(60.0f);
        cameraComponent->camera()->setNearClip(0.1f);
        cameraComponent->camera()->setFarClip(100.0f);
        cameraComponent->setToneMapping(TONEMAP_ACES);
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 1.5f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-38.0f, 8.0f));
            controls->setZoomRange(Vector2(2.0f, 7.0f));
            controls->storeResetState();
        }
        auto rendering = cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 6;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.35f;
        cameraComponent->setRendering(rendering);

        applySettings();
        spdlog::info("reflection-box: R reflections, [ ] frequency, P box projection, 5/6 reflectivity, - = floor "
                     "roughness, 9/0 floor metalness, 1/2 sphere roughness, 3/4 bumpiness, M metal, Space pause");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        const auto step = [](float& value, const float delta) { value = std::clamp(value + delta, 0.0f, 1.0f); };
        switch (event.key.key) {
        case SDLK_R: _reflections = !_reflections; break;
        case SDLK_LEFTBRACKET: _updateFrequency = std::max(0, _updateFrequency - 1); break;
        case SDLK_RIGHTBRACKET: _updateFrequency = std::min(30, _updateFrequency + 1); break;
        case SDLK_P: _boxProjection = !_boxProjection; break;
        case SDLK_5: step(_reflectivity, -0.05f); break;
        case SDLK_6: step(_reflectivity, 0.05f); break;
        case SDLK_MINUS: step(_floorRoughness, -0.05f); break;
        case SDLK_EQUALS: step(_floorRoughness, 0.05f); break;
        case SDLK_9: step(_floorMetalness, -0.05f); break;
        case SDLK_0: step(_floorMetalness, 0.05f); break;
        case SDLK_1: step(_sphereRoughness, -0.05f); break;
        case SDLK_2: step(_sphereRoughness, 0.05f); break;
        case SDLK_3: step(_sphereBumpiness, -0.05f); break;
        case SDLK_4: step(_sphereBumpiness, 0.05f); break;
        case SDLK_M: _sphereMetalness = _sphereMetalness > 0.5f ? 0.0f : 1.0f; break;
        case SDLK_SPACE: _animate = !_animate; break;
        default: return false;
        }
        applySettings();
        return true;
    }

    void update(const float dt) override
    {
        if (_animate) {
            _time += dt;
        }

        // The orb circles the pedestals, bobbing up and down, and the torus spins.
        const float orbAngle = _time * 0.4f;
        _orb->setPosition(std::sin(orbAngle) * 5.5f, 2.2f + std::sin(_time * 1.1f) * 0.6f, 0.5f + std::cos(orbAngle) * 3.2f);
        _torusPivot->setLocalEulerAngles(0.0f, _time * 40.0f, 0.0f);

        // The probe renders every few frames, or just once, while the reflections are on.
        if (_reflections) {
            bool render = false;
            if (_updateFrequency == 0) {
                render = !_probeRendered;
            } else if (--_framesToProbe <= 0) {
                render = true;
                _framesToProbe = _updateFrequency;
            }
            if (render) {
                _probe->requestBake();
                _captureThisFrame = true;
                _probeRendered = true;
            }
        }
    }

    void postRender() override
    {
        // A capture just rendered: build its mips, then prefilter it into the atlas the
        // materials reflect until the next one. An HDR capture is linear already.
        _probe->update();
        if (_captureThisFrame) {
            _captureThisFrame = false;
            EnvLighting::generateAtlas(device().get(), _probe->cubemap(), _envAtlas.get(), 512, 1024, 2048,
                !_probe->hdr());
        }
    }

    void destroy() override
    {
        for (auto* material : _reflective) {
            material->setEnvAtlas(nullptr);
        }
        _probe.reset();
    }

private:
    // A metallic material reflecting the probe's atlas; the gloss is the roughness, as the
    // room's materials have it.
    std::shared_ptr<StandardMaterial> createReflectiveMaterial(const Color& color)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setUseMetalness(true);
        material->setMetalness(1.0f);
        material->setGlossInvert(true);
        _materials.push_back(material);
        _reflective.push_back(material.get());
        return material;
    }

    std::shared_ptr<StandardMaterial> createEmissiveMaterial(const Color& color, const float intensity)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setUseLighting(false);
        material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        material->setEmissive(color);
        material->setEmissiveIntensity(intensity);
        _materials.push_back(material);
        return material;
    }

    void applySettings()
    {
        _floorMaterial->setGloss(1.0f - _floorRoughness);   // a glTF material: gloss = 1 - roughness
        _floorMaterial->setMetalness(_floorMetalness);
        _sphereMaterial->setGloss(_sphereRoughness);
        _sphereMaterial->setMetalness(_sphereMetalness);
        _sphereMaterial->setBumpiness(_sphereBumpiness);
        const BoundingBox roomBox(Vector3(0.0f, 3.25f, 0.0f), Vector3(8.0f, 3.25f, 9.0f));
        for (auto* material : _reflective) {
            material->setEnvAtlas(_reflections ? _envAtlas.get() : nullptr);
            material->setCubeMapProjection(_boxProjection ? CUBEPROJ_BOX : CUBEPROJ_NONE);
            material->setCubeMapProjectionBox(roomBox);
            material->setReflectivity(_reflectivity);
        }
    }

    std::unique_ptr<Asset> _room;
    std::unique_ptr<Asset> _statue;
    std::unique_ptr<Asset> _normal;
    std::unique_ptr<Asset> _video;
    std::shared_ptr<Texture> _envAtlas;
    std::unique_ptr<ReflectionProbe> _probe;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<StandardMaterial*> _reflective;
    StandardMaterial* _floorMaterial = nullptr;
    std::shared_ptr<StandardMaterial> _sphereMaterial;
    std::shared_ptr<StandardMaterial> _torusMaterial;
    Entity* _torusPivot = nullptr;
    Entity* _orb = nullptr;

    bool _reflections = true;
    int _updateFrequency = 10;
    bool _boxProjection = true;
    float _reflectivity = 1.0f;
    float _floorRoughness = 0.05f;
    float _floorMetalness = 0.7f;
    float _sphereRoughness = 0.05f;
    float _sphereMetalness = 1.0f;
    float _sphereBumpiness = 0.0f;
    bool _animate = true;
    float _time = 0.0f;
    int _framesToProbe = 0;
    bool _probeRendered = false;
    bool _captureThisFrame = false;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionBoxExample)
