// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Port of upstream render-targets/render-to-texture.
//
// A drone circles the statue in the room (area-lights-room.glb), its camera rendering into a
// texture shown on a hanging screen and in the corner of the view. The texture (512 x 256, 2x MSAA,
// mipmapped, as the screen shows it smaller) is the target of the drone's camera (World and Skybox,
// ACES, fov 55, rendered first); the drone and the four-sided screen above the statue are on an
// Excluded layer only the main camera renders, so they are not in the drone's picture. The drone
// orbits closer and further, higher and lower, looking at the statue, its rotors spinning and its
// LED blinking, and its camera switches between perspective and orthographic (height 2.2) every
// 5 seconds. Sparks rise around the statue (300 additive particles), the room and its objects
// reflect the room's own environment atlas box projected onto it, a red and a chrome sphere stand
// on the side pedestals, and the helipad sky shows through the windows. One shadowed omni light
// lights both layers.
//
// Keys stand in for upstream's controls panel:
//   P            the drone camera's projection: alternate / perspective / orthographic
//   , / .        the animation speed -/+ 0.5 (0 - 3)
//   O            the picture in the corner on or off
//
// DEVIATIONS:
// - The corner picture is a TexturePreviewPass (examples/texturePreview.h), upstream's
//   TextureRenderer, at the same place.
// - The glTF materials are found from the instantiated meshes rather than the container's list.
// - The texture is RGBA8 holding the drone camera's gamma-encoded output (upstream's is sRGB);
//   the screen material decodes it as every 8-bit colour map.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "../texturePreview.h"
#include "core/math/curve.h"
#include "core/math/curveSet.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The drone and the screen: rendered only by the main camera.
    constexpr int LAYERID_EXCLUDED = 100;

    enum class Projection
    {
        Alternate,
        Perspective,
        Orthographic,
    };
}

class RenderToTextureExample final : public ExampleApp
{
public:
    RenderToTextureExample() : ExampleApp({.title = "Render to Texture"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        _room = std::make_unique<Asset>("room", AssetType::CONTAINER, assetPath("models/area-lights-room.glb"));
        _statue = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _drone = std::make_unique<Asset>("drone", AssetType::CONTAINER, assetPath("models/drone.glb"));
        _spark = std::make_unique<Asset>("spark", AssetType::TEXTURE, assetPath("textures/spark.png"));
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        // The room rendered into a cubemap from its centre, prefiltered into an atlas.
        _roomEnvAtlas = std::make_unique<Asset>("room-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/area-lights-room-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* roomContainer = _room->resourceAs<ContainerResource>();
        auto* statueContainer = _statue->resourceAs<ContainerResource>();
        auto* droneContainer = _drone->resourceAs<ContainerResource>();
        Texture* spark = _spark->resourceAs<Texture>();
        Texture* helipad = _helipad->resourceAs<Texture>();
        if (!roomContainer || !statueContainer || !droneContainer || !spark || !helipad ||
            !_roomEnvAtlas->resourceAs<Texture>()) {
            spdlog::error("render-to-texture: an asset did not load");
            return false;
        }

        // The layers: World and Skybox for both cameras, Excluded for the main one only.
        const auto layers = scene()->layers();
        layers->insert(std::make_shared<Layer>("Excluded", LAYERID_EXCLUDED), 1);
        const std::vector<int> excluded = {LAYERID_EXCLUDED};

        // The sky, at the full resolution of its atlas, through the windows and the skylight.
        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxMip(0);
        scene()->setExposure(0.6f);

        // The room, with the statue on the middle pedestal and a sphere on each side one.
        Entity* room = roomContainer->instantiateRenderEntity();
        room->setEngine(engine());
        root()->addChild(room);
        Entity* statue = statueContainer->instantiateRenderEntity();
        statue->setEngine(engine());
        statue->setLocalScale(0.15f, 0.15f, 0.15f);
        statue->setLocalPosition(0.0f, 0.36f, 0.0f);
        statue->setLocalEulerAngles(0.0f, 30.0f, 0.0f);
        root()->addChild(statue);
        for (Entity* model : {room, statue}) {
            for (auto* render : model->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
            }
            addReflectionsToModel(model);
        }

        const struct
        {
            float x;
            Color color;
            float gloss;
            float metalness;
        } spheres[] = {
            {4.0f, Color(0.7f, 0.02f, 0.02f, 1.0f), 0.85f, 0.0f},
            {-4.0f, Color(0.95f, 0.95f, 0.95f, 1.0f), 0.95f, 1.0f},
        };
        for (const auto& sphere : spheres) {
            auto material = createMaterial(sphere.color, sphere.gloss, sphere.metalness);
            createPrimitive("sphere", material.get(), Vector3(sphere.x, 1.15f, 0.8f), Vector3(1.2f, 1.2f, 1.2f),
                {LAYERID_WORLD});
        }

        // The glowing doors in the back wall.
        for (const auto& [x, color] : {std::pair{-4.5f, Color(1.0f, 0.42f, 0.12f, 1.0f)},
                 std::pair{0.0f, Color(0.25f, 0.8f, 1.0f, 1.0f)}, std::pair{4.5f, Color(1.0f, 0.8f, 0.35f, 1.0f)}}) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissive(color);
            material->setEmissiveIntensity(3.0f);
            material->setUseLighting(false);
            _materials.push_back(material);
            createPrimitive("box", material.get(), Vector3(x, 1.7f, -9.45f), Vector3(1.7f, 3.3f, 0.02f), {LAYERID_WORLD});
        }

        createSparks(spark);

        // The light of the room, on both layers, so it lights the drone too.
        auto* light = new Entity();
        light->setName("Light");
        light->setEngine(engine());
        if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_OMNI);
            lc->setColor(Color(1.0f, 0.95f, 0.9f, 1.0f));
            lc->setIntensity(2.0f);
            lc->setRange(20.0f);
            lc->setCastShadows(true);
            lc->setShadowBias(0.2f);
            lc->setShadowNormalBias(0.05f);
            lc->setLayers({LAYERID_WORLD, LAYERID_EXCLUDED});
        }
        light->setLocalPosition(1.0f, 5.5f, 4.0f);
        root()->addChild(light);

        // The texture the drone camera renders into: depth, 2x MSAA, mipmapped, as the screen
        // shows it smaller than its size.
        TextureOptions textureOptions;
        textureOptions.name = "DroneCamera";
        textureOptions.width = 512;
        textureOptions.height = 256;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = true;
        _texture = std::make_shared<Texture>(device().get(), textureOptions);
        _texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device().get();
        targetOptions.name = "Drone Camera";
        targetOptions.colorBuffer = _texture.get();
        targetOptions.depth = true;
        targetOptions.samples = 2;
        auto renderTarget = device()->createRenderTarget(targetOptions);

        // The drone, carrying the camera that renders into the texture, first each frame.
        _droneEntity = createCamera(Vector3(0.0f, 2.5f, 4.5f));
        _droneEntity->setName("Drone");
        _droneCamera = _droneEntity->findComponent<CameraComponent>();
        _droneCamera->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        _droneCamera->setToneMapping(TONEMAP_ACES);
        _droneCamera->setPriority(-1);
        _droneCamera->camera()->setFov(55.0f);
        _droneCamera->camera()->setNearClip(0.1f);
        _droneCamera->camera()->setOrthoHeight(2.2f);
        _droneCamera->camera()->setRenderTarget(renderTarget);

        Entity* droneModel = droneContainer->instantiateRenderEntity();
        droneModel->setEngine(engine());
        _droneEntity->addChild(droneModel);
        for (auto* render : droneModel->findComponents<RenderComponent>()) {
            render->setLayers(excluded);
        }
        addReflectionsToModel(droneModel);
        for (GraphNode* node : droneModel->find([](GraphNode* n) { return n && n->name().rfind("Rotor", 0) == 0; })) {
            const Vector3 p = node->localPosition();
            const float product = p.getX() * p.getZ();
            _rotors.push_back({node, product > 0.0f ? 1.0f : (product < 0.0f ? -1.0f : 0.0f)});
        }
        _led = dynamic_cast<Entity*>(droneModel->findByName("LED"));

        // The screen hanging from the ceiling above the statue, a display on each of its sides.
        auto* screen = new Entity();
        screen->setName("Screen");
        screen->setEngine(engine());
        screen->setLocalPosition(0.0f, 4.5f, 0.0f);
        root()->addChild(screen);
        auto darkMaterial = createMaterial(Color(0.03f, 0.03f, 0.04f, 1.0f), 0.8f, 0.8f);
        addChildPrimitive(screen, "box", darkMaterial.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(2.8f, 1.5f, 2.8f), excluded);
        for (const float x : {-0.9f, 0.9f}) {
            for (const float z : {-0.9f, 0.9f}) {
                addChildPrimitive(screen, "cylinder", darkMaterial.get(), Vector3(x, 1.375f, z),
                    Vector3(0.03f, 1.25f, 0.03f), excluded);
            }
        }
        _screenMaterial = std::make_shared<StandardMaterial>();
        _screenMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _screenMaterial->setEmissiveMap(_texture.get());
        _screenMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _screenMaterial->setEmissiveIntensity(1.5f);
        _screenMaterial->setUseLighting(false);
        for (int i = 0; i < 4; ++i) {
            // The plane faces up its Y axis, turned to face out of each side.
            auto* side = new Entity();
            side->setEngine(engine());
            side->setLocalEulerAngles(0.0f, static_cast<float>(i) * 90.0f, 0.0f);
            screen->addChild(side);
            Entity* display = addChildPrimitive(side, "plane", _screenMaterial.get(), Vector3(0.0f, 0.0f, 1.42f),
                Vector3(2.6f, 1.0f, 1.3f), excluded);
            display->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        }

        // The main camera: every layer, the picture in the corner on the UI layer.
        Entity* camera = createCamera(Vector3(1.3f, 2.8f, 6.8f));
        camera->setName("MainCamera");
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_EXCLUDED, LAYERID_SKYBOX, LAYERID_IMMEDIATE, LAYERID_UI});
        cameraComponent->setToneMapping(TONEMAP_ACES);
        cameraComponent->camera()->setFov(60.0f);
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 2.7f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-38.0f, 8.0f));
            controls->setZoomRange(Vector2(2.0f, 7.0f));
            controls->storeResetState();
        }

        // The picture of the drone camera in the bottom right corner: (0.7, 0.7) to (0.98, 0.98)
        // of the view, from its top left, as upstream's TextureRenderer draws it.
        _preview = std::make_shared<TexturePreviewPass>(device(), _texture.get());
        _preview->init(nullptr);
        engine()->renderer()->addAppendPass(_preview);

        update(0.0f);
        spdlog::info("render-to-texture: P projection (alternate / perspective / orthographic), , . speed, O picture");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_P:
            _projection = _projection == Projection::Alternate ? Projection::Perspective
                : _projection == Projection::Perspective ? Projection::Orthographic : Projection::Alternate;
            break;
        case SDLK_COMMA: _speed = std::max(0.0f, _speed - 0.5f); break;
        case SDLK_PERIOD: _speed = std::min(3.0f, _speed + 0.5f); break;
        case SDLK_O: _overlay = !_overlay; break;
        default: return false;
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * _speed;

        // The drone orbits the statue, closer and further, higher and lower, looking at it, but
        // never over it.
        const float angle = _time * 0.4f;
        const float distance = 4.5f + std::sin(_time * 0.23f) * 1.2f;
        _droneEntity->setLocalPosition(std::sin(angle) * distance, 2.5f + std::sin(_time * 0.31f) * 0.6f,
            std::cos(angle) * distance);
        _droneEntity->lookAt(Vector3(0.0f, 1.5f, 0.0f));
        for (const auto& [rotor, direction] : _rotors) {
            rotor->setLocalEulerAngles(0.0f, direction * _time * 2000.0f, 0.0f);
        }
        if (_led) {
            _led->setEnabled(static_cast<int>(std::floor(_time * 2.0f)) % 2 == 0);
        }

        // The drone camera switches between the projections every 5 seconds.
        _projectionTime += dt;
        bool orthographic = _projection == Projection::Orthographic;
        if (_projection == Projection::Alternate) {
            orthographic = static_cast<int>(std::floor(_projectionTime / 5.0f)) % 2 == 1;
        }
        _droneCamera->camera()->setProjection(orthographic ? ProjectionType::Orthographic : ProjectionType::Perspective);

        if (_preview) {
            _preview->setEnabled(_overlay);
            const auto [width, height] = device()->size();
            const float fw = static_cast<float>(width);
            const float fh = static_cast<float>(height);
            _preview->setViewport(Vector4(0.7f * fw, 0.7f * fh, 0.28f * fw, 0.28f * fh));
            _preview->setScissor(_preview->viewport());
        }
    }

    void destroy() override
    {
        if (_preview) {
            engine()->renderer()->removeAppendPass(_preview);
            _preview.reset();
        }
    }

private:
    // A material reflects the room's atlas, box projected onto its inside.
    void addReflections(StandardMaterial* material) const
    {
        material->setEnvAtlas(_roomEnvAtlas->resourceAs<Texture>());
        material->setCubeMapProjection(CUBEPROJ_BOX);
        material->setCubeMapProjectionBox(BoundingBox(Vector3(0.0f, 3.25f, 0.0f), Vector3(8.0f, 3.25f, 9.0f)));
    }

    void addReflectionsToModel(Entity* model) const
    {
        for (auto* render : model->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                if (auto* material = dynamic_cast<StandardMaterial*>(meshInstance ? meshInstance->material() : nullptr)) {
                    addReflections(material);
                }
            }
        }
    }

    std::shared_ptr<StandardMaterial> createMaterial(const Color& color, const float gloss, const float metalness)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setGloss(gloss);
        material->setUseMetalness(true);
        material->setMetalness(metalness);
        addReflections(material.get());
        _materials.push_back(material);
        return material;
    }

    Entity* addChildPrimitive(Entity* parent, const char* type, Material* material, const Vector3& position,
        const Vector3& scale, const std::vector<int>& layers)
    {
        Entity* entity = createPrimitive(type, material, position, scale, layers);
        parent->addChild(entity);
        entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
        return entity;
    }

    // Sparks rising around the statue.
    void createSparks(Texture* spark)
    {
        auto* entity = new Entity();
        entity->setName("Sparks");
        entity->setEngine(engine());
        root()->addChild(entity);
        auto* particles = static_cast<ParticleSystemComponent*>(entity->addComponent<ParticleSystemComponent>());
        if (!particles) {
            return;
        }
        auto& o = particles->options();
        o.numParticles = 300;
        o.lifetime = 2.5f;
        o.lifetime2 = 2.5f;
        o.rate = 0.01f;
        o.emitterShape = ParticleEmitterShape::EMITTERSHAPE_SPHERE;
        o.emitterRadius = 1.2f;
        o.colorMap = spark;
        o.blendType = ParticleBlendType::BLEND_ADDITIVE;
        o.depthWrite = false;
        o.scaleGraph = Curve(std::vector<float>{0.0f, 0.15f, 1.0f, 0.03f});
        o.alphaGraph = Curve(std::vector<float>{0.0f, 0.0f, 0.1f, 1.0f, 1.0f, 0.0f});
        o.colorGraph = CurveSet({{0.0f, 0.2f, 1.0f, 1.0f}, {0.0f, 0.9f, 1.0f, 0.3f}, {0.0f, 1.0f, 1.0f, 1.0f}});
        o.velocityGraph = CurveSet({{0.0f, -0.3f}, {0.0f, 1.0f, 1.0f, 2.0f}, {0.0f, -0.3f}});
        o.velocityGraph2 = CurveSet({{0.0f, 0.3f}, {0.0f, 1.6f, 1.0f, 2.6f}, {0.0f, 0.3f}});
        particles->apply();
        particles->play();
    }

    std::unique_ptr<Asset> _room;
    std::unique_ptr<Asset> _statue;
    std::unique_ptr<Asset> _drone;
    std::unique_ptr<Asset> _spark;
    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _roomEnvAtlas;

    std::shared_ptr<Texture> _texture;
    std::shared_ptr<StandardMaterial> _screenMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<TexturePreviewPass> _preview;

    Entity* _droneEntity = nullptr;
    CameraComponent* _droneCamera = nullptr;
    std::vector<std::pair<GraphNode*, float>> _rotors;
    Entity* _led = nullptr;

    Projection _projection = Projection::Alternate;
    float _speed = 1.0f;
    bool _overlay = true;
    float _time = 0.0f;
    float _projectionTime = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(RenderToTextureExample)
