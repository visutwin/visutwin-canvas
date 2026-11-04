// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/reflection-planar.
//
// The polished floor of a room reflects it with a planar reflection. A camera mirrored by the
// floor (the PlanarRenderer script) renders the room into a mipmapped, high dynamic range texture
// each frame, and the floor material reflects it (StandardMaterial::setPlanarReflectionMap) in
// place of its environment reflection: unlike the box-projected cubemap baked from the empty
// room, it reflects the statue, the drone circling it and the spinning gold torus exactly, also
// where they touch the floor. A rougher floor reads a smaller mip, blurring the reflection. The
// room, the statue and the drone reflect the room's own environment atlas, box projected onto
// the room; the helipad sky shows through the windows. Three glowing doors with spot lights and
// an omni light over the statue light it; the camera frame (4x MSAA, ACES) adds bloom and a
// vignette.
//
// Keys stand in for upstream's controls panel:
//   P            planar reflection / the cubemap (the reflection camera off)
//   [ / ]        the reflection's resolution -/+ 0.1 (0.1 - 1)
//   - / =        the floor's roughness -/+ 0.05 (0 - 0.6, written to its gloss, as upstream)
//   T            show the reflection texture in the corner
//   , / .        the animation speed -/+ 0.5 (0 - 3)
//
// DEVIATIONS:
// - The texture preview is a TexturePreviewPass (examples/texturePreview.h), upstream's
//   TextureRenderer, at the same place in the corner.
// - The glTF materials are gathered from the instantiated meshes rather than the container's
//   material list.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../texturePreview.h"
#include "extras/script/cameraControls.h"
#include "extras/script/planarRenderer.h"
#include "framework/assets/asset.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/script/scriptComponent.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The layer of the floor, rendered only by the main camera: a surface cannot be in the
    // reflection it displays.
    constexpr int LAYERID_FLOOR = 100;
}

class ReflectionPlanarExample final : public ExampleApp
{
public:
    ReflectionPlanarExample() : ExampleApp({.title = "Planar Reflection"}) {}

protected:
    bool create() override
    {
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        // The empty room rendered into a cubemap from its centre, prefiltered into an
        // environment atlas: the reflection for each level of roughness.
        _roomEnvAtlas = std::make_unique<Asset>("room-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/area-lights-room-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _room = std::make_unique<Asset>("room", AssetType::CONTAINER, assetPath("models/area-lights-room.glb"));
        _statue = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _drone = std::make_unique<Asset>("drone", AssetType::CONTAINER, assetPath("models/drone.glb"));

        // The layers: World and Skybox, rendered by both cameras, and the floor's own.
        const auto layers = scene()->layers();
        layers->insert(std::make_shared<Layer>("Floor", LAYERID_FLOOR), 1);
        const std::vector<int> lightLayers = {LAYERID_WORLD, LAYERID_FLOOR};

        // The sky, at the full resolution of its atlas, through the windows and the skylight.
        if (Texture* helipad = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(helipad);
        }
        scene()->setSkyboxMip(0);
        scene()->setExposure(0.6f);

        // The room, with the statue on the pedestal in the middle.
        auto* roomContainer = _room->resourceAs<ContainerResource>();
        auto* statueContainer = _statue->resourceAs<ContainerResource>();
        auto* droneContainer = _drone->resourceAs<ContainerResource>();
        if (!roomContainer || !statueContainer || !droneContainer) {
            spdlog::error("reflection-planar: a model did not load");
            return false;
        }
        Entity* room = roomContainer->instantiateRenderEntity();
        room->setEngine(engine());
        root()->addChild(room);

        Entity* statue = statueContainer->instantiateRenderEntity();
        statue->setEngine(engine());
        statue->setLocalScale(0.15f, 0.15f, 0.15f);
        statue->setLocalPosition(0.0f, 0.36f, 0.0f);
        statue->setLocalEulerAngles(0.0f, 30.0f, 0.0f);
        root()->addChild(statue);

        // The drone, flying around the statue; it casts but does not receive shadows.
        _droneEntity = droneContainer->instantiateRenderEntity();
        _droneEntity->setEngine(engine());
        root()->addChild(_droneEntity);

        for (Entity* model : {room, statue, _droneEntity}) {
            for (auto* render : model->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setReceiveShadows(model != _droneEntity);
            }
        }

        // The rotors spin, the ones on opposite corners the same way, and the LED blinks.
        for (GraphNode* node : _droneEntity->find([](GraphNode* n) { return n && n->name().rfind("Rotor", 0) == 0; })) {
            const Vector3 p = node->localPosition();
            const float product = p.getX() * p.getZ();
            _rotors.push_back({node, product > 0.0f ? 1.0f : (product < 0.0f ? -1.0f : 0.0f)});
        }
        _led = dynamic_cast<Entity*>(_droneEntity->findByName("LED"));

        // Every glTF material reflects the room, box projected onto it.
        std::vector<StandardMaterial*> gltfMaterials;
        for (Entity* model : {room, statue, _droneEntity}) {
            for (auto* render : model->findComponents<RenderComponent>()) {
                for (auto* meshInstance : render->meshInstances()) {
                    auto* material = dynamic_cast<StandardMaterial*>(meshInstance ? meshInstance->material() : nullptr);
                    if (material && std::find(gltfMaterials.begin(), gltfMaterials.end(), material) == gltfMaterials.end()) {
                        gltfMaterials.push_back(material);
                    }
                }
            }
        }
        for (auto* material : gltfMaterials) {
            addReflections(material);
            if (material->name() == "Floor") {
                _floorMaterial = material;
            }
        }

        // A chrome sphere on the left pedestal, and a gold torus spinning on the right one.
        _chrome = makeMetal(Color(0.95f, 0.95f, 0.95f, 1.0f), 0.95f);
        createPrimitive("sphere", _chrome.get(), Vector3(-4.0f, 1.15f, 0.8f), Vector3(1.2f, 1.2f, 1.2f), {LAYERID_WORLD});
        _gold = makeMetal(Color(1.0f, 0.77f, 0.34f, 1.0f), 0.75f);
        _torusPivot = new Entity();
        _torusPivot->setName("Torus");
        _torusPivot->setEngine(engine());
        _torusPivot->setLocalPosition(4.0f, 1.29f, 0.8f);
        root()->addChild(_torusPivot);
        Entity* torus = createPrimitive("torus", _gold.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(1.5f, 1.5f, 1.5f),
            {LAYERID_WORLD});
        _torusPivot->addChild(torus);
        torus->setLocalPosition(0.0f, 0.0f, 0.0f);
        torus->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

        // The glowing doors in the back wall, each with a spot light shining into the room.
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
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissive(door.color);
            material->setEmissiveIntensity(4.0f);
            material->setUseLighting(false);
            _doorMaterials.push_back(material);
            createPrimitive("box", material.get(), Vector3(door.x, 1.7f, -9.45f), Vector3(1.7f, 3.3f, 0.02f),
                {LAYERID_WORLD});

            // A spot light shines down its -Y axis, so it is turned to face the room.
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
            }
            light->setLocalPosition(door.x, 1.7f, -9.2f);
            light->setLocalEulerAngles(-90.0f, 0.0f, 0.0f);
            root()->addChild(light);
        }

        // The light of the room, shining on the statue from the front.
        auto* roomLight = new Entity();
        roomLight->setName("Light");
        roomLight->setEngine(engine());
        if (auto* lc = static_cast<LightComponent*>(roomLight->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_OMNI);
            lc->setLayers(lightLayers);
            lc->setColor(Color(1.0f, 0.95f, 0.9f, 1.0f));
            lc->setIntensity(2.0f);
            lc->setRange(20.0f);
            lc->setCastShadows(true);
            lc->setShadowBias(0.2f);
            lc->setShadowNormalBias(0.05f);
        }
        roomLight->setLocalPosition(1.0f, 5.5f, 4.0f);
        root()->addChild(roomLight);

        // The floor, on its own layer.
        if (auto* floorEntity = dynamic_cast<Entity*>(room->findByName("RoomFloor"))) {
            for (auto* render : floorEntity->findComponents<RenderComponent>()) {
                render->setLayers({LAYERID_FLOOR});
            }
        }
        if (!_floorMaterial) {
            spdlog::error("reflection-planar: the room has no 'Floor' material");
            return false;
        }
        // Mostly metallic, so the reflection shows over its diffuse lighting.
        _floorMaterial->setMetalness(0.7f);

        // The main camera, low in the room, where the floor reflects the most.
        Entity* camera = createCamera(Vector3(1.5f, 1.8f, 6.8f));
        camera->setName("MainCamera");
        _cameraComponent = camera->findComponent<CameraComponent>();
        _cameraComponent->setLayers({LAYERID_WORLD, LAYERID_FLOOR, LAYERID_SKYBOX, LAYERID_UI});
        _cameraComponent->camera()->setFov(60.0f);
        _cameraComponent->camera()->setNearClip(0.1f);
        _cameraComponent->camera()->setFarClip(100.0f);
        _cameraComponent->setToneMapping(TONEMAP_ACES);

        // It orbits all the way round; the pitch and zoom ranges keep it inside the room.
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 1.2f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-45.0f, 5.0f));
            controls->setZoomRange(Vector2(3.5f, 7.0f));
            controls->storeResetState();
        }

        // Bloom makes the doors glow, and their reflections.
        auto rendering = _cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 6;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.35f;
        _cameraComponent->setRendering(rendering);

        // The reflection camera, rendering World and Skybox into the reflection texture first
        // (its priority is lower). The script mirrors the main camera by the floor's plane, and
        // makes the texture mipmapped, for the blurred reflections of rough floors, and of high
        // dynamic range, so the doors' reflections are as bright as the doors.
        _reflectionCamera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _reflectionCamera->setName("ReflectionCamera");
        if (auto* reflectionComponent = _reflectionCamera->findComponent<CameraComponent>()) {
            reflectionComponent->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
            reflectionComponent->setPriority(-1);
            reflectionComponent->setToneMapping(TONEMAP_ACES);
        }
        _reflectionCamera->addComponent<ScriptComponent>();
        _planarRenderer = _reflectionCamera->script()->create<PlanarRenderer>();
        _planarRenderer->setSceneCameraEntity(camera);
        _planarRenderer->setMipmaps(true);
        _planarRenderer->setHdr(true);
        _planarRenderer->setPlane(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));

        applySettings();
        spdlog::info("reflection-planar: P planar/cubemap, [ ] resolution, - = roughness, T texture, , . speed");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_P: _planar = !_planar; break;
        case SDLK_LEFTBRACKET: _resolution = std::max(0.1f, _resolution - 0.1f); break;
        case SDLK_RIGHTBRACKET: _resolution = std::min(1.0f, _resolution + 0.1f); break;
        case SDLK_MINUS: _roughness = std::max(0.0f, _roughness - 0.05f); break;
        case SDLK_EQUALS: _roughness = std::min(0.6f, _roughness + 0.05f); break;
        case SDLK_T: _showTexture = !_showTexture; break;
        case SDLK_COMMA: _speed = std::max(0.0f, _speed - 0.5f); break;
        case SDLK_PERIOD: _speed = std::min(3.0f, _speed + 0.5f); break;
        default: return false;
        }
        applySettings();
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * _speed;

        // The drone circles the statue, dipping toward the floor and back up, facing where it
        // flies, its nose down and banking into the turn.
        const float angle = _time * 0.5f;
        constexpr float radius = 2.6f;
        const float height = 1.1f + std::sin(_time * 0.7f) * 0.5f;
        _droneEntity->setLocalPosition(std::sin(angle) * radius, height, std::cos(angle) * radius);
        _droneEntity->lookAt(std::sin(angle + 0.1f) * radius, height, std::cos(angle + 0.1f) * radius);
        _droneEntity->rotateLocal(-8.0f, 0.0f, 15.0f);
        for (const auto& [rotor, direction] : _rotors) {
            rotor->setLocalEulerAngles(0.0f, direction * _time * 2000.0f, 0.0f);
        }
        if (_led) {
            _led->setEnabled(static_cast<int>(std::floor(_time * 2.0f)) % 2 == 0);
        }

        // The torus spins on its pedestal.
        _torusPivot->setLocalEulerAngles(0.0f, _time * 40.0f, 0.0f);

        // The reflection camera, after the main camera moved this frame. The floor reflects its
        // texture through its view-projection; with the camera off there is no texture and the
        // floor falls back to its environment atlas.
        Texture* texture = _planarRenderer->frameUpdate();
        _floorMaterial->setPlanarReflectionMap(texture);
        _floorMaterial->setPlanarReflectionMatrix(_planarRenderer->viewProjectionMatrix());

        updatePreview(texture && _showTexture ? texture : nullptr);
    }

    void destroy() override
    {
        if (_preview) {
            engine()->renderer()->removeAppendPass(_preview);
            _preview.reset();
        }
        if (_floorMaterial) {
            _floorMaterial->setPlanarReflectionMap(nullptr);
        }
    }

private:
    // A material reflects the room, box projected onto its inside (the cubemap was rendered
    // from the centre of the room), and is lit by it.
    void addReflections(StandardMaterial* material) const
    {
        if (Texture* atlas = _roomEnvAtlas->resourceAs<Texture>()) {
            material->setEnvAtlas(atlas);
        }
        material->setCubeMapProjection(CUBEPROJ_BOX);
        material->setCubeMapProjectionBox(BoundingBox(Vector3(0.0f, 3.25f, 0.0f), Vector3(8.0f, 3.25f, 9.0f)));
    }

    std::shared_ptr<StandardMaterial> makeMetal(const Color& diffuse, const float gloss) const
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(diffuse);
        material->setUseMetalness(true);
        material->setMetalness(1.0f);
        material->setGloss(gloss);
        addReflections(material.get());
        return material;
    }

    void applySettings()
    {
        // With the cubemap, the reflection camera need not render.
        if (_reflectionCamera) {
            if (auto* component = _reflectionCamera->findComponent<CameraComponent>()) {
                component->setEnabled(_planar);
            }
        }
        if (_planarRenderer) {
            _planarRenderer->setScale(_resolution);
        }
        if (_floorMaterial) {
            _floorMaterial->setGloss(_roughness);   // as upstream, which writes the roughness to gloss
        }
    }

    // The reflection texture in the corner, at upstream's TextureRenderer place (centre 0.7,
    // 0.7 in clip space, 0.28 across).
    void updatePreview(Texture* texture)
    {
        if (!texture) {
            if (_preview) {
                _preview->setEnabled(false);
            }
            return;
        }
        if (!_preview) {
            _preview = std::make_shared<TexturePreviewPass>(device(), texture);
            _preview->init(nullptr);
            engine()->renderer()->addAppendPass(_preview);
        }
        _preview->setTexture(texture);
        _preview->setEnabled(true);
        const auto [width, height] = device()->size();
        const float fw = static_cast<float>(width);
        const float fh = static_cast<float>(height);
        _preview->setViewport(Vector4(0.78f * fw, 0.08f * fh, 0.14f * fw, 0.14f * fh));
        _preview->setScissor(_preview->viewport());
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _roomEnvAtlas;
    std::unique_ptr<Asset> _room;
    std::unique_ptr<Asset> _statue;
    std::unique_ptr<Asset> _drone;

    Entity* _droneEntity = nullptr;
    std::vector<std::pair<GraphNode*, float>> _rotors;
    Entity* _led = nullptr;
    Entity* _torusPivot = nullptr;
    StandardMaterial* _floorMaterial = nullptr;
    std::shared_ptr<StandardMaterial> _chrome;
    std::shared_ptr<StandardMaterial> _gold;
    std::vector<std::shared_ptr<StandardMaterial>> _doorMaterials;
    CameraComponent* _cameraComponent = nullptr;
    Entity* _reflectionCamera = nullptr;
    PlanarRenderer* _planarRenderer = nullptr;
    std::shared_ptr<TexturePreviewPass> _preview;

    bool _planar = true;
    float _resolution = 0.5f;
    float _roughness = 0.1f;
    bool _showTexture = false;
    float _speed = 1.0f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionPlanarExample)
