// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// 2D-cartesian animation blend-tree example — mirrors upstream's
// `animation/blend-trees-2d-cartesian` example. The skinned bitmoji character is
// driven through an AnimComponent state graph whose single "Emote" state hosts a
// BLEND_2D_CARTESIAN blend tree. FOUR leaf clips sit at 2D points on the blend
// plane, and TWO float parameters ("speedX", "speedY") pick the blend point;
// AnimBlendTreeCartesian2D computes the per-clip weights via gradient-band
// interpolation of the 2D positions.
//
// The idle/walk/run/win-dance clips live in SEPARATE GLB files
// (assets/animations/bitmoji/*.glb) and are retargeted onto the bitmoji rig by
// node name via DefaultAnimBinder — cross-GLB retargeting, same as the 1D
// anim-stategraph example this is based on.
//
// Blend tree layout (see build below):
//
//        speedY
//          ^
//    walk (0,1) ── run (1,1)
//          │          │
//    idle (0,0) ── dance (1,0) ─> speedX
//
// Auto-demo: the 2D blend point sweeps a circle around the centre of the point
// cloud (a Lissajous-ish path), so the character continuously blends between all
// four clips across the 2D plane.
//
// Keys: 1 = idle (0,0), 2 = walk (0,1), 3 = run (1,1), 4 = dance (1,0),
//       5 = centre (0.5,0.5 — even four-way blend). Any key stops the auto-demo.
//
// DEVIATION / NOTE: this is a valid 2D-cartesian blend demonstration, but the
// clip set here is forward-locomotion + a dance emote (idle/walk/run/win-dance),
// NOT upstream's directional strafe set (idle/eager/walk/dance arranged around
// the origin). So the 2D axes are illustrative of the blend feature rather than
// semantically directional (there is no left/right strafe clip). The blending
// math (AnimBlendTreeCartesian2D) is exercised identically regardless.
//
#include <algorithm>
#include <cmath>
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

class BlendTrees2dExample final: public ExampleApp
{
public:
    BlendTrees2dExample()
        : ExampleApp({.title = "2D Cartesian Blend Tree", .width = 1200, .height = 800}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        // Match upstream blend-trees-2d-cartesian: exposure 2, full-intensity skybox,
        // default linear tone mapping (no ACES — it darkens midtones noticeably).
        scene()->setSkyboxMip(2);
        scene()->setExposure(2.0f);

        _modelAsset = std::make_unique<Asset>(
            "bitmoji", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _idleAnimAsset = std::make_unique<Asset>(
            "idleAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/idle.glb"));
        _walkAnimAsset = std::make_unique<Asset>(
            "walkAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/walk.glb"));
        _runAnimAsset = std::make_unique<Asset>(
            "runAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/run.glb"));
        _danceAnimAsset = std::make_unique<Asset>(
            "danceAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/win-dance.glb"));
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
        auto danceTrack = loadFirstAnimTrack(_danceAnimAsset.get());

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

        // -----------------------------------------------------------------------
        // Build the anim state graph with a single 2D-CARTESIAN blend tree:
        //
        //   START ──► Emote { BLEND_2D_CARTESIAN, params [speedX, speedY],
        //                     leaves: Idle(0,0) Walk(0,1) Run(1,1) Dance(1,0) }
        //
        // The two float parameters name the X/Y axes of the blend plane; the leaf
        // `point`s are the clips' 2D positions. AnimBlendTreeCartesian2D derives each
        // clip's weight from where (speedX, speedY) falls among those points.
        // -----------------------------------------------------------------------
        if (!idleTrack || !walkTrack || !runTrack || !danceTrack) {
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
        // Start the blend point at the centre of the cloud → even four-way blend.
        stateGraph.addFloatParameter("speedX", 0.5f);
        stateGraph.addFloatParameter("speedY", 0.5f);

        auto& layer = stateGraph.addLayer("base");

        // The 2D-cartesian blend tree. TWO driving parameters, and each leaf
        // child carries a distinct Vector2 `point` on the blend plane.
        AnimBlendTreeDesc emoteTree;
        emoteTree.type = AnimBlendType::BLEND_2D_CARTESIAN;
        emoteTree.parameters = {"speedX", "speedY"};
        emoteTree.syncAnimations = true;
        emoteTree.children.push_back(AnimBlendTreeDesc{.name = "Idle",  .point = Vector2(0.0f, 0.0f)});
        emoteTree.children.push_back(AnimBlendTreeDesc{.name = "Walk",  .point = Vector2(0.0f, 1.0f)});
        emoteTree.children.push_back(AnimBlendTreeDesc{.name = "Run",   .point = Vector2(1.0f, 1.0f)});
        emoteTree.children.push_back(AnimBlendTreeDesc{.name = "Dance", .point = Vector2(1.0f, 0.0f)});
        layer.states.push_back(AnimStateDesc{"Emote", 1.0f, true, emoteTree});

        layer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Emote"});

        _animComp = static_cast<AnimComponent*>(modelEntity->addComponent<AnimComponent>());
        _animComp->loadStateGraph(stateGraph);
        // Assign tracks parsed from the SEPARATE animation GLBs — retargeted onto
        // the bitmoji rig by node name — to each blend-tree leaf via "State.Leaf".
        _animComp->assignAnimation("Emote.Idle", idleTrack);
        _animComp->assignAnimation("Emote.Walk", walkTrack);
        _animComp->assignAnimation("Emote.Run", runTrack);
        _animComp->assignAnimation("Emote.Dance", danceTrack);

        spdlog::info("2D blend tree loaded: Emote{{Idle(0,0) Walk(0,1) Run(1,1) Dance(1,0)}} "
                     "driven by (speedX, speedY) — keys 1-5 set the blend point");

        // -----------------------------------------------------------------------
        // Ground plane (playcanvas-grey texture, like the upstream demos)
        // -----------------------------------------------------------------------
        _groundMaterial = std::make_shared<StandardMaterial>();
        if (const auto groundTexRes = _groundTexAsset->resource();
            groundTexRes && std::holds_alternative<Texture*>(*groundTexRes)) {
            _groundMaterial->setDiffuseMap(std::get<Texture*>(*groundTexRes));
        } else {
            spdlog::warn("Ground texture failed to load — using flat grey");
        }
        // Darken the ground so it stays below 1.0 under exposure 2 + linear
        // tonemap — a blown-out white ground clips the soft shadow away
        // (shadow only removes the directional term; IBL keeps the rest lit).
        _groundMaterial->setDiffuse(Color(0.32f, 0.32f, 0.32f, 1.0f));

        // NOTE: the ground must stay a shadow CASTER — the directional shadow
        // camera tightens its far plane to the caster AABB union, and a
        // receiver-only ground would fall beyond it (shadow clips along a
        // light-space plane through the character).
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
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f, true);
        if (auto* keyLightComp = keyLight->findComponent<LightComponent>()) {
            // Upstream shadow settings: distance 6, bias 0.02, normalOffsetBias 0.02,
            // single cascade (upstream default) — the tight fit keeps the whole
            // character silhouette in one cascade instead of splitting it.
            keyLightComp->setShadowResolution(2048);
            keyLightComp->setShadowDistance(20.0f);
            keyLightComp->setNumCascades(1);
            keyLightComp->setShadowBias(0.02f);
            keyLightComp->setShadowNormalBias(0.02f);
        }

        // -----------------------------------------------------------------------
        // Camera with orbit controls
        // -----------------------------------------------------------------------
        _camDistance = std::max(radius * 2.8f, 5.0f);
        auto* cameraEntity = createCamera(_center + Vector3(0.0f, radius * 0.5f, _camDistance));

        if (auto* cameraComp = cameraEntity->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
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

        spdlog::info("Controls: keys 1-5 pick blend corners/centre; LMB/RMB orbit, "
                     "Shift/MMB pan, Wheel zoom, F focus, R reset, Esc quit");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        if (event.key.key >= SDLK_1 && event.key.key <= SDLK_5 && _animComp) {
            // Manual key targets: the four leaf corners + the centre.
            static constexpr float keyX[] = {0.0f, 0.0f, 1.0f, 1.0f, 0.5f};
            static constexpr float keyY[] = {0.0f, 1.0f, 1.0f, 0.0f, 0.5f};
            static constexpr const char* keyLabels[] = {
                "idle (0,0)", "walk (0,1)", "run (1,1)", "dance (1,0)", "centre (0.5,0.5) even blend"};

            const int i = event.key.key - SDLK_1;
            _autoDemo = false;  // manual control takes over
            _animComp->setFloat("speedX", keyX[i]);
            _animComp->setFloat("speedY", keyY[i]);
            spdlog::info("blend point = ({:.2f}, {:.2f}) → {} (state: {})",
                keyX[i], keyY[i], keyLabels[i],
                _animComp->baseLayer() ? _animComp->baseLayer()->activeState() : "?");
            return true;
        }
        if (event.key.key == SDLK_F && _controls) {
            _controls->focus(_center, _camDistance);
            return true;
        }
        return false;
    }

    // engine->update(dt) advances the anim component, which recomputes the 2D
    // blend weights from (speedX, speedY) and composites the four clips; GPU
    // skinning follows the resulting bones each frame.
    void update(const float dt) override
    {
        // Auto-demo: sweep the 2D blend point around a circle centred on the point
        // cloud (0.5, 0.5) until the user presses a number key. The path visits the
        // neighbourhood of every leaf, so the character continuously cross-blends.
        if (!_autoDemo || !_animComp) {
            return;
        }
        _demoTime += dt;
        // Circle of radius 0.5 centred at (0.5, 0.5): X uses cos, Y uses sin
        // at a slightly different rate so the sweep isn't a perfect loop
        // (Lissajous-like), touching each corner's neighbourhood in turn.
        constexpr float kCenter = 0.5f;
        constexpr float kRadius = 0.5f;
        const float sx = kCenter + kRadius * std::cos(_demoTime * 0.6f);
        const float sy = kCenter + kRadius * std::sin(_demoTime * 0.8f);
        _animComp->setFloat("speedX", sx);
        _animComp->setFloat("speedY", sy);
    }

    void destroy() override
    {
        spdlog::info("*** 2D-Cartesian Blend-Tree Example Finished ***");
    }

private:
    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _idleAnimAsset;
    std::unique_ptr<Asset> _walkAnimAsset;
    std::unique_ptr<Asset> _runAnimAsset;
    std::unique_ptr<Asset> _danceAnimAsset;
    std::unique_ptr<Asset> _groundTexAsset;
    std::unique_ptr<Asset> _envAtlas;

    std::shared_ptr<StandardMaterial> _groundMaterial;
    AnimComponent* _animComp = nullptr;
    CameraControls* _controls = nullptr;

    Vector3 _center;
    float _camDistance = 5.0f;

    bool _autoDemo = true;
    float _demoTime = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(BlendTrees2dExample)
