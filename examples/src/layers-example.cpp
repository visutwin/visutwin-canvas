// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream's graphics/layers: composes the frame from multiple layers.
// An X-Ray layer draws a character silhouette through walls using a greater depth
// test, a Character layer renders the walking character on top of it, and a Front
// layer with depth clearing keeps a held item from clipping into the scene.
// Keys 1-5 toggle the individual layers (upstream has UI checkboxes).
//
// Asset credit (apartment.glb): "Mirror's Edge Apartment - Interior Scene" by
// Aurelien Martel, CC BY-NC 4.0 — see assets/models/apartment.txt.
//
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/graphics/depthState.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Layer ids for the three layers this example adds. Upstream allocates them
// automatically; here they just have to be unique and outside the built-in range.
constexpr int LAYERID_XRAY = 20;
constexpr int LAYERID_CHARACTER = 21;
constexpr int LAYERID_FRONT = 22;

class LayersExample final: public ExampleApp
{
public:
    LayersExample(): ExampleApp({.title = "Layers", .width = 1200, .height = 800}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        spdlog::info("*** Layers Example ***");

        // Setup skydome for environment lighting
        // JS: app.scene.envAtlas = assets.helipad.resource; app.scene.exposure = 1.2;
        scene()->setExposure(1.2f);

        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        const auto helipadResource = _helipad->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        // ------ Layer setup ------

        _layers = scene()->layers();
        _worldLayer = _layers->getLayerById(LAYERID_WORLD);
        if (!_worldLayer) {
            spdlog::error("World layer missing from the default composition");
            return false;
        }

        // The 'X-Ray' layer renders after the World layer's opaque meshes. Meshes placed in it
        // draw on top of what the World layer rendered, which this example combines with a
        // greater depth test to show a character silhouette through walls. It is inserted before
        // the World layer's transparent sublayer, so that transparent meshes (which do not write
        // depth) still render on top of the silhouette.
        _xrayLayer = std::make_shared<Layer>("X-Ray", LAYERID_XRAY);
        _layers->insert(_xrayLayer, _layers->getTransparentIndex(_worldLayer));

        // The 'Character' layer renders the character normally, after the X-Ray layer. Keeping
        // the character out of the World layer means the x-ray depth test only compares against
        // the world geometry, and rendering it after the X-Ray layer paints the visible parts of
        // the character over the silhouette.
        _characterLayer = std::make_shared<Layer>("Character", LAYERID_CHARACTER);
        _layers->insert(_characterLayer, _layers->getTransparentIndex(_worldLayer));

        // The 'Front' layer renders last and clears the depth buffer before drawing, so its
        // meshes are never clipped by the world geometry - useful for held items or 3D HUD
        // elements.
        _frontLayer = std::make_shared<Layer>("Front", LAYERID_FRONT);
        _frontLayer->setClearDepthBuffer(true);
        _layers->pushOpaque(_frontLayer);
        _layers->pushTransparent(_frontLayer);

        // ------ Scene setup ------

        _apartmentAsset = std::make_unique<Asset>(
            "apartment", AssetType::CONTAINER, assetPath("models/apartment.glb"));
        _bitmojiAsset = std::make_unique<Asset>(
            "bitmoji", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _walkAsset = std::make_unique<Asset>(
            "walk", AssetType::CONTAINER, assetPath("animations/bitmoji/walk.glb"));
        _cubeAsset = std::make_unique<Asset>(
            "cube", AssetType::CONTAINER, assetPath("models/playcanvas-cube.glb"));

        // Create an instance of the apartment and add it to the scene
        auto* apartmentContainer = loadContainer(_apartmentAsset);
        if (!apartmentContainer) {
            return false;
        }
        auto* apartmentEntity = apartmentContainer->instantiateRenderEntity();
        apartmentEntity->setEngine(engine());
        root()->addChild(apartmentEntity);
        apartmentEntity->setLocalScale(30.0f, 30.0f, 30.0f);

        // Add a concrete pillar between the camera and the walking path, to demonstrate the
        // x-ray effect when the character walks behind it
        _pillarMaterial = std::make_shared<StandardMaterial>();
        _pillarMaterial->setDiffuse(Color(0.58f, 0.57f, 0.55f));
        _pillarMaterial->setGloss(0.3f);

        auto* pillar = createPrimitive("box", _pillarMaterial.get(),
            Vector3(-160.0f, 120.0f, -62.0f), Vector3(20.0f, 240.0f, 20.0f));
        pillar->setName("Pillar");

        // A root entity moved along a path each frame, carrying both copies of the walking character
        _walkerRoot = new Entity();
        _walkerRoot->setName("WalkerRoot");
        _walkerRoot->setEngine(engine());
        root()->addChild(_walkerRoot);

        // The character is rendered normally in the Character layer
        auto* bitmojiContainer = loadContainer(_bitmojiAsset);
        auto* walkContainer = loadContainer(_walkAsset);
        if (!bitmojiContainer || !walkContainer || walkContainer->animTracks().empty()) {
            spdlog::error("Character or walk animation failed to load");
            return false;
        }
        auto* walker = bitmojiContainer->instantiateRenderEntity();
        walker->setEngine(engine());
        _walkerRoot->addChild(walker);
        walker->setLocalScale(60.0f, 60.0f, 60.0f);

        // JS: walker.addComponent('anim', { activate: true });
        //     walker.anim.assignAnimation('Walk', assets.walk.resource.animations[0].resource);
        // DEVIATION: the anim component here is state-graph driven, so the single looping clip
        // is expressed as a one-state graph rather than a bare assignAnimation call.
        if (auto* legacyAnim = walker->findComponent<AnimationComponent>()) {
            legacyAnim->setPlaying(false);
            legacyAnim->setEnabled(false);
        }
        {
            AnimStateGraph stateGraph;
            auto& animLayer = stateGraph.addLayer("locomotion");
            animLayer.states.push_back(AnimStateDesc{"Walk"});
            animLayer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Walk"});

            auto* animComp = static_cast<AnimComponent*>(walker->addComponent<AnimComponent>());
            animComp->loadStateGraph(stateGraph);
            animComp->assignAnimation("Walk", walkContainer->animTracks().begin()->second);
        }

        setEntityLayers(walker, {LAYERID_CHARACTER});

        // Flat emissive material with a greater depth test - it only passes where the character
        // is behind already rendered geometry, so the silhouette shows only when occluded
        _xrayMaterial = std::make_shared<StandardMaterial>();
        _xrayMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f));
        _xrayMaterial->setEmissive(Color(1.2f, 0.2f, 0.25f));
        {
            auto xrayDepth = std::make_shared<DepthState>();
            xrayDepth->setFunc(CompareFunction::Greater);
            xrayDepth->setDepthWrite(false);
            _xrayMaterial->setDepthState(xrayDepth);
        }

        // Render the character a second time into the X-Ray layer, by adding mesh instances to
        // it directly. These share the mesh, node and skin instance with the normal copy, so
        // they render in the exact same pose, and the greater depth test only passes where the
        // character is hidden behind the world geometry.
        std::vector<MeshInstance*> xrayMeshInstances;
        for (auto* render : walker->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                if (!meshInstance || !meshInstance->mesh()) {
                    continue;
                }
                auto* xrayInstance = new MeshInstance(
                    meshInstance->mesh(), _xrayMaterial.get(), meshInstance->node());
                xrayInstance->setSkinInstance(meshInstance->skinInstanceShared());
                xrayInstance->setMorphInstance(meshInstance->morphInstanceShared());
                xrayInstance->setCastShadow(false);
                xrayMeshInstances.push_back(xrayInstance);
            }
        }
        _xrayLayer->addMeshInstances(xrayMeshInstances);
        spdlog::info("X-Ray layer: {} mesh instances sharing the character's pose", xrayMeshInstances.size());

        // Create an Entity with a camera component, rendering the default layers as well as the
        // three newly created layers
        auto* cameraEntity = createCamera(Vector3(-318.0f, 114.0f, -59.0f));
        cameraEntity->setName("Camera");
        auto* cameraComp = cameraEntity->findComponent<CameraComponent>();
        cameraComp->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        cameraComp->camera()->setFarClip(1500.0f);
        cameraComp->camera()->setFov(80.0f);
        cameraComp->setLayers({
            LAYERID_WORLD,
            LAYERID_DEPTH,
            LAYERID_SKYBOX,
            LAYERID_IMMEDIATE,
            LAYERID_UI,
            LAYERID_XRAY,
            LAYERID_CHARACTER,
            LAYERID_FRONT
        });

        // Add orbit camera controls, focused on the center of the walking path
        // JS: cameraControls.focusPoint = new Vec3(-60, 45, -15);
        //     cameraControls.zoomRange = new Vec2(30, 600);
        if (auto* cameraControls = addOrbitControls(cameraEntity, Vector3(-60.0f, 45.0f, -15.0f))) {
            cameraControls->setZoomRange(Vector2(30.0f, 600.0f));
        }

        // Cube in the Front layer, attached to the camera like a held item. Because the Front
        // layer clears the depth buffer, the cube is never clipped by nearby walls.
        auto* cubeContainer = loadContainer(_cubeAsset);
        if (!cubeContainer) {
            return false;
        }
        _cubeEntity = cubeContainer->instantiateRenderEntity();
        _cubeEntity->setEngine(engine());
        cameraEntity->addChild(_cubeEntity);
        setEntityLayers(_cubeEntity, {LAYERID_FRONT});
        _cubeEntity->setLocalScale(12.0f, 12.0f, 12.0f);
        _cubeEntity->setLocalPosition(28.0f, -22.0f, -60.0f);

        // ------ UI handling ------
        // Upstream exposes five checkboxes; here they are keys.
        spdlog::info("Keys: 1 World  2 X-Ray  3 Character  4 Front  5 Front clear-depth  |  ESC quits");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_1:
            _worldEnabled = !_worldEnabled;
            _worldLayer->setEnabled(_worldEnabled);
            _layers->markDirty();
            spdlog::info("World layer: {}", _worldEnabled ? "on" : "off");
            return true;
        case SDLK_2:
            _xrayEnabled = !_xrayEnabled;
            _xrayLayer->setEnabled(_xrayEnabled);
            _layers->markDirty();
            spdlog::info("X-Ray layer: {}", _xrayEnabled ? "on" : "off");
            return true;
        case SDLK_3:
            _characterEnabled = !_characterEnabled;
            _characterLayer->setEnabled(_characterEnabled);
            _layers->markDirty();
            spdlog::info("Character layer: {}", _characterEnabled ? "on" : "off");
            return true;
        case SDLK_4:
            _frontEnabled = !_frontEnabled;
            _frontLayer->setEnabled(_frontEnabled);
            _layers->markDirty();
            spdlog::info("Front layer: {}", _frontEnabled ? "on" : "off");
            return true;
        case SDLK_5:
            _frontClearDepth = !_frontClearDepth;
            _frontLayer->setClearDepthBuffer(_frontClearDepth);
            _layers->markDirty();
            spdlog::info("Front layer clear depth: {}", _frontClearDepth ? "on" : "off");
            return true;
        default:
            return false;
        }
    }

    void update(const float dt) override
    {
        // walk in an ellipse on the open floor - the walk animation roughly matches this speed
        _angle += dt * 0.6f;

        _walkerRoot->setLocalPosition(
            kWalkCenter.getX() + std::sin(_angle) * kWalkRadiusX,
            kWalkCenter.getY(),
            kWalkCenter.getZ() + std::cos(_angle) * kWalkRadiusZ
        );

        // face the walking direction (tangent of the ellipse) - lookAt points the entity's -Z
        // axis at the target, and the model faces +Z, so look at the point behind the character
        _walkerRoot->lookAt(Vector3(
            kWalkCenter.getX() + std::sin(_angle - 0.1f) * kWalkRadiusX,
            kWalkCenter.getY(),
            kWalkCenter.getZ() + std::cos(_angle - 0.1f) * kWalkRadiusZ
        ));

        // slowly spin the held cube
        _cubeEntity->rotateLocal(0.0f, 20.0f * dt, 0.0f);
    }

    void destroy() override
    {
        spdlog::info("*** Layers Example Finished ***");
    }

private:
    static GlbContainerResource* loadContainer(const std::unique_ptr<Asset>& asset)
    {
        if (!asset) {
            return nullptr;
        }
        const auto resource = asset->resource();
        if (!resource || !std::holds_alternative<ContainerResource*>(*resource)) {
            spdlog::error("GLB '{}' failed to load as a container", asset->name());
            return nullptr;
        }
        return dynamic_cast<GlbContainerResource*>(std::get<ContainerResource*>(*resource));
    }

    // Put every RenderComponent under an entity into the given layer.
    static void setEntityLayers(Entity* entity, const std::vector<int>& layers)
    {
        if (!entity) {
            return;
        }
        for (auto* render : entity->findComponents<RenderComponent>()) {
            render->setLayers(layers);
        }
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _apartmentAsset;
    std::unique_ptr<Asset> _bitmojiAsset;
    std::unique_ptr<Asset> _walkAsset;
    std::unique_ptr<Asset> _cubeAsset;

    std::shared_ptr<StandardMaterial> _pillarMaterial;
    std::shared_ptr<StandardMaterial> _xrayMaterial;

    std::shared_ptr<LayerComposition> _layers;
    std::shared_ptr<Layer> _worldLayer;
    std::shared_ptr<Layer> _xrayLayer;
    std::shared_ptr<Layer> _characterLayer;
    std::shared_ptr<Layer> _frontLayer;

    Entity* _walkerRoot = nullptr;
    Entity* _cubeEntity = nullptr;

    bool _worldEnabled = true;
    bool _xrayEnabled = true;
    bool _characterEnabled = true;
    bool _frontEnabled = true;
    bool _frontClearDepth = true;

    const Vector3 kWalkCenter{-60.0f, 1.0f, -35.0f};
    static constexpr float kWalkRadiusX = 90.0f;
    static constexpr float kWalkRadiusZ = 50.0f;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(LayersExample)
