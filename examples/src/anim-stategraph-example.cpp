// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Anim state-graph example — mirrors upstream's `locomotion` example. The
// skinned bitmoji character is driven through an AnimComponent state graph:
// an "Idle" state transitions into a 1D "Locomotion" blend tree (Walk <-> Run)
// based on a float "speed" parameter. The idle/walk/run animation clips live in
// SEPARATE GLB files (assets/animations/bitmoji/*.glb) and are retargeted onto
// the bitmoji rig by node name via DefaultAnimBinder — cross-GLB retargeting.
//
// Keys: 1 = idle (speed 0), 2 = walk (1.0), 3 = run (2.0), 4 = 50/50 blend (1.5).
//
#include <algorithm>
#include <memory>
#include <string>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "core/shape/boundingBox.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/parsers/glbContainerResource.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Load a container GLB and return its first parsed animation track (each
// bitmoji animation GLB contains exactly one clip). Returns nullptr on failure.
std::shared_ptr<AnimTrack> loadFirstAnimTrack(Asset* asset)
{
    if (!asset) {
        return nullptr;
    }
    const auto resource = asset->resource();
    if (!resource || !std::holds_alternative<ContainerResource*>(*resource)) {
        spdlog::error("Animation GLB '{}' failed to load as a container", asset->name());
        return nullptr;
    }
    auto* glb = dynamic_cast<GlbContainerResource*>(std::get<ContainerResource*>(*resource));
    if (!glb || glb->animTracks().empty()) {
        spdlog::error("Animation GLB '{}' contains no animation tracks", asset->name());
        return nullptr;
    }
    const auto& tracks = glb->animTracks();
    const auto& [name, track] = *tracks.begin();
    spdlog::info("Loaded animation track '{}' from '{}' ({} curves)",
        name, asset->name(), track ? track->curves().size() : 0);
    return track;
}

class AnimStateGraphExample final: public ExampleApp
{
public:
    AnimStateGraphExample()
        : ExampleApp({.title = "A Windy Day", .width = 1200, .height = 800}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        spdlog::info("*** Animation Example — A Windy Day ***");

        scene()->setSkyboxMip(2);
        scene()->setSkyboxIntensity(0.7f);
        scene()->setExposure(1.0f);
        scene()->setToneMapping(TONEMAP_ACES);

        // Character model (skinned rig) + the separate per-clip animation GLBs. Each
        // animation GLB's curves target the shared bitmoji rig by node name, so the
        // tracks retarget onto the instantiated character via DefaultAnimBinder.
        _modelAsset = std::make_unique<Asset>(
            "bitmoji", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _idleAnimAsset = std::make_unique<Asset>(
            "idleAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/idle.glb"));
        _walkAnimAsset = std::make_unique<Asset>(
            "walkAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/walk.glb"));
        _runAnimAsset = std::make_unique<Asset>(
            "runAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/run.glb"));
        _groundTexAsset = std::make_unique<Asset>(
            "playcanvas-grey", AssetType::TEXTURE, assetPath("textures/playcanvas-grey.png"));
        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );

        // Load environment atlas for IBL
        if (const auto envAtlasResource = _envAtlas->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
        } else {
            spdlog::warn("Failed to load environment atlas — continuing without IBL");
        }

        // -----------------------------------------------------------------------
        // Load the bitmoji character model
        // -----------------------------------------------------------------------
        spdlog::info("Loading bitmoji character GLB...");
        const auto resource = _modelAsset->resource();
        if (!resource) {
            spdlog::error("GLB load failed: asset resource is null");
            return false;
        }
        if (!std::holds_alternative<ContainerResource*>(*resource)) {
            spdlog::error("GLB load failed: expected ContainerResource");
            return false;
        }

        auto* container = std::get<ContainerResource*>(*resource);
        if (!container) {
            spdlog::error("GLB load failed: container payload is null");
            return false;
        }

        auto* modelEntity = container->instantiateRenderEntity();
        if (!modelEntity) {
            spdlog::error("GLB instantiate failed");
            return false;
        }
        modelEntity->setEngine(engine());
        root()->addChild(modelEntity);

        // Load the separate per-clip animation GLBs and extract their tracks. These
        // will be retargeted onto the bitmoji rig by node name.
        auto idleTrack = loadFirstAnimTrack(_idleAnimAsset.get());
        auto walkTrack = loadFirstAnimTrack(_walkAnimAsset.get());
        auto runTrack = loadFirstAnimTrack(_runAnimAsset.get());

        // Log model stats
        {
            int renderComps = 0, meshInsts = 0;
            for (auto* render : RenderComponent::instances()) {
                if (!render || !render->entity()) continue;
                auto* owner = render->entity();
                if (owner != modelEntity && !owner->isDescendantOf(modelEntity)) continue;
                renderComps++;
                meshInsts += static_cast<int>(render->meshInstances().size());
            }
            spdlog::info("Model loaded: {} render components, {} mesh instances",
                renderComps, meshInsts);
        }

        // Build the anim state graph (mirrors upstream locomotion):
        //
        //   START ──► Idle ──(speed >= 0.5)──► Locomotion (1D blend: Walk@1 .. Run@2)
        //              ▲                            │
        //              └───────(speed < 0.5)────────┘
        //
        if (!idleTrack || !walkTrack || !runTrack) {
            spdlog::error("Missing one or more animation tracks — cannot build state graph");
            return false;
        }

        // The bitmoji GLB has no embedded animations, but disable any legacy
        // AnimationComponent just in case so it doesn't fight the state graph.
        if (auto* legacyAnim = modelEntity->findComponent<AnimationComponent>()) {
            legacyAnim->setPlaying(false);
            legacyAnim->setEnabled(false);
        }

        AnimStateGraph stateGraph;
        stateGraph.addFloatParameter("speed", 0.0f);

        auto& layer = stateGraph.addLayer("locomotion");
        layer.states.push_back(AnimStateDesc{"Idle"});

        AnimBlendTreeDesc locomotionTree;
        locomotionTree.type = AnimBlendType::BLEND_1D;
        locomotionTree.parameters = {"speed"};
        locomotionTree.syncAnimations = true;
        locomotionTree.children.push_back(AnimBlendTreeDesc{.name = "Walk", .point = Vector2(1.0f, 0.0f)});
        locomotionTree.children.push_back(AnimBlendTreeDesc{.name = "Run", .point = Vector2(2.0f, 0.0f)});
        layer.states.push_back(AnimStateDesc{"Locomotion", 1.0f, true, locomotionTree});

        layer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Idle"});
        layer.transitions.push_back(AnimTransitionDesc{
            .from = "Idle", .to = "Locomotion", .time = 0.4f,
            .conditions = {{"speed", AnimPredicate::GREATER_THAN_EQUAL_TO, 0.5f}}});
        layer.transitions.push_back(AnimTransitionDesc{
            .from = "Locomotion", .to = "Idle", .time = 0.4f,
            .conditions = {{"speed", AnimPredicate::LESS_THAN, 0.5f}}});

        _animComp = static_cast<AnimComponent*>(modelEntity->addComponent<AnimComponent>());
        _animComp->loadStateGraph(stateGraph);
        // Assign tracks parsed from the SEPARATE animation GLBs — retargeted onto
        // the bitmoji rig by node name.
        _animComp->assignAnimation("Idle", idleTrack);
        _animComp->assignAnimation("Locomotion.Walk", walkTrack);
        _animComp->assignAnimation("Locomotion.Run", runTrack);

        spdlog::info("State graph loaded: states [Idle, Locomotion(Walk|Run)] — keys 1/2/3/4 set speed");

        // -----------------------------------------------------------------------
        // Ground plane (playcanvas-grey texture, like the upstream locomotion demo)
        // -----------------------------------------------------------------------
        _groundMaterial = std::make_shared<StandardMaterial>();
        if (const auto groundTexRes = _groundTexAsset->resource();
            groundTexRes && std::holds_alternative<Texture*>(*groundTexRes)) {
            _groundMaterial->setDiffuseMap(std::get<Texture*>(*groundTexRes));
        } else {
            _groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.5f, 1.0f));
            spdlog::warn("Ground texture failed to load — using flat grey");
        }
        createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(15.0f, 1.0f, 15.0f));

        // Auto-frame the model
        const BoundingBox modelBounds = entityBounds(modelEntity);
        _center = modelBounds.center();
        const Vector3 halfExt = modelBounds.halfExtents();
        const float radius = std::max(halfExt.length(), 0.5f);
        spdlog::info("Model bounds: center=({:.2f}, {:.2f}, {:.2f}), half=({:.2f}, {:.2f}, {:.2f}), radius={:.2f}",
            _center.getX(), _center.getY(), _center.getZ(),
            halfExt.getX(), halfExt.getY(), halfExt.getZ(), radius);

        // -----------------------------------------------------------------------
        // Lights
        // -----------------------------------------------------------------------
        auto* keyLight = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 0.97f, 0.92f), 1.5f, true);
        if (auto* keyLightComp = keyLight->findComponent<LightComponent>()) {
            keyLightComp->setShadowResolution(2048);
            keyLightComp->setShadowDistance(std::max(radius * 4.0f, 100.0f));
            keyLightComp->setShadowBias(0.3f);
            keyLightComp->setShadowNormalBias(0.05f);
        }

        createDirectionalLight(Vector3(-20.0f, -150.0f, 0.0f), Color(0.65f, 0.75f, 1.0f), 0.5f);

        // -----------------------------------------------------------------------
        // Camera with orbit controls
        // -----------------------------------------------------------------------
        _camDistance = std::max(radius * 2.8f, 5.0f);
        auto* cameraEntity = createCamera(_center + Vector3(0.0f, radius * 0.5f, _camDistance));

        if (auto* cameraComp = cameraEntity->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->camera()->setClearColor(Color(0.05f, 0.05f, 0.08f, 1.0f));
            cameraComp->camera()->setFov(55.0f);
            cameraComp->camera()->setNearClip(std::max(0.01f, radius * 0.005f));
            cameraComp->camera()->setFarClip(std::max(500.0f, radius * 20.0f));
        }

        _controls = addOrbitControls(cameraEntity, _center);
        _controls->setMoveSpeed(radius);
        _controls->setMoveFastSpeed(radius * 2.0f);
        _controls->setMoveSlowSpeed(radius * 0.5f);
        _controls->setOrbitDistance(_camDistance);
        _controls->storeResetState();

        spdlog::info("Controls: LMB/RMB orbit, Shift/MMB pan, Wheel zoom, F focus, R reset, Esc quit");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        if (event.key.key >= SDLK_1 && event.key.key <= SDLK_4 && _animComp) {
            static constexpr float speeds[] = {0.0f, 1.0f, 2.0f, 1.5f};
            const float speed = speeds[event.key.key - SDLK_1];
            _autoDemo = false;  // manual control takes over
            _animComp->setFloat("speed", speed);
            spdlog::info("speed = {:.1f} (state: {})", speed,
                _animComp->baseLayer() ? _animComp->baseLayer()->activeState() : "?");
            return true;
        }
        if (event.key.key == SDLK_F && _controls) {
            _controls->focus(_center, _camDistance);
            return true;
        }
        return false;
    }

    // engine->update(dt) advances the animation, which drives the joint entities;
    // GPU skinning follows the bones each frame.
    void update(const float dt) override
    {
        // Auto-demo: cycle the speed parameter until the user presses a number key.
        if (!_autoDemo || !_animComp) {
            return;
        }
        static constexpr float demoSpeeds[] = {0.0f, 2.0f, 1.5f, 1.0f};
        static constexpr const char* demoLabels[] = {
            "idle (Survey)", "run", "walk/run 50/50 blend", "walk"};

        _demoTime += dt;
        const int step = static_cast<int>(_demoTime / 4.0f) % 4;
        if (step != _demoStep) {
            _demoStep = step;
            _animComp->setFloat("speed", demoSpeeds[step]);
            spdlog::info("[auto-demo] speed = {:.1f} → {} (state: {})", demoSpeeds[step],
                demoLabels[step],
                _animComp->baseLayer() ? _animComp->baseLayer()->activeState() : "?");
        }
    }

    void destroy() override
    {
        spdlog::info("*** Animation Example Finished ***");
    }

private:
    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _idleAnimAsset;
    std::unique_ptr<Asset> _walkAnimAsset;
    std::unique_ptr<Asset> _runAnimAsset;
    std::unique_ptr<Asset> _groundTexAsset;
    std::unique_ptr<Asset> _envAtlas;

    std::shared_ptr<StandardMaterial> _groundMaterial;
    AnimComponent* _animComp = nullptr;
    CameraControls* _controls = nullptr;

    Vector3 _center;
    float _camDistance = 5.0f;

    bool _autoDemo = true;
    float _demoTime = 0.0f;
    int _demoStep = -1;
};

VISUTWIN_EXAMPLE_MAIN(AnimStateGraphExample)
