// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream animation/layer-masks.
//
// The bitmoji character with two animation layers and no state graph. The base layer plays the
// full body, idle or walking; an "UpperBody" layer, ADDITIVE and masked to the spine above the
// first spine joint (and everything beneath it), adds an upper-body clip on top: eager, idle or
// a dance. The two layers' weights are 1 - blend and blend (0.5 to begin with). A layer added
// this way starts in no state; the controls start the base layer in Idle and the upper body in
// Eager, which together hold the arms down (either alone at half weight leaves them halfway to
// the T-pose rest). The skeleton is drawn over the character, each bone coloured by the layers
// driving it: red by the base layer's weight, blue by the upper body's.
//
// Keys stand in for upstream's controls panel:
//   1 / 2      full body: Idle / Walk
//   3 / 4 / 5  upper body: Eager / Idle / Dance
//   A          upper body blend type: additive / overwrite
//   M          upper body mask on / off
//   [ / ]      blend -/+ 0.1
//   S          skeleton on / off
//
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "../animClips.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

namespace
{
    // The upper body: the second spine joint and everything beneath it.
    const char* const kUpperBodyRoot = "RootNode/AVATAR/C_spine0001_bind_JNT/C_spine0002_bind_JNT";
}

class LayerMasksExample final : public ExampleApp
{
public:
    LayerMasksExample() : ExampleApp({.title = "Layer Masks"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _idleAsset = std::make_unique<Asset>("idleAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/idle.glb"));
        _eagerAsset = std::make_unique<Asset>("idleEagerAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/idle-eager.glb"));
        _walkAsset = std::make_unique<Asset>("walkAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/walk.glb"));
        _danceAsset = std::make_unique<Asset>("danceAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/win-dance.glb"));
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});

        // The sky dome
        scene()->setExposure(2.0f);
        scene()->setSkyboxMip(2);
        if (Texture* helipad = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(helipad);
        }

        auto* cameraEntity = createCamera(Vector3(0.0f, 0.75f, 3.0f));
        if (auto* camera = cameraEntity->findComponent<CameraComponent>(); camera && camera->camera()) {
            camera->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        }
        auto* lightEntity = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f, true);
        if (auto* light = lightEntity->findComponent<LightComponent>()) {
            light->setShadowNormalBias(0.02f);
            light->setShadowType(ShadowType::SHADOW_PCF5_32F);
            light->setShadowDistance(6.0f);
            light->setShadowResolution(2048);
            light->setShadowBias(0.02f);
        }

        auto* container = _modelAsset->resourceAs<ContainerResource>();
        _model = container ? container->instantiateRenderEntity() : nullptr;
        const auto idle = firstAnimTrack(_idleAsset.get());
        const auto walk = firstAnimTrack(_walkAsset.get());
        const auto dance = firstAnimTrack(_danceAsset.get());
        const auto eager = firstAnimTrack(_eagerAsset.get());
        if (!_model || !idle || !walk || !dance || !eager) {
            spdlog::error("layer-masks needs bitmoji.glb and its clips");
            return false;
        }
        _model->setEngine(engine());
        _anim = static_cast<AnimComponent*>(_model->addComponent<AnimComponent>());
        root()->addChild(_model);

        // The full body layer: assigning animations with no state graph makes the base layer,
        // which plays the first one
        _anim->assignAnimation("Idle", idle);
        _anim->assignAnimation("Walk", walk);
        _anim->baseLayer()->setWeight(1.0f - _blend);

        // The upper body layer, additive, masked to the upper body
        _upperBody = _anim->addLayer("UpperBody", _blend, {}, AnimLayerBlendType::ADDITIVE, {kUpperBodyRoot});
        _upperBody->assignAnimation("Eager", eager);
        _upperBody->assignAnimation("Idle", idle);
        _upperBody->assignAnimation("Dance", dance);

        // The controls' initial states: setting them transitions each layer, so the upper body
        // starts in Eager
        _anim->baseLayer()->transition("Idle", 0.4f);
        _upperBody->transition("Eager", 0.4f);

        // The nodes the clips animate: only these have a bone drawn
        for (const auto& track : {idle, walk, dance, eager}) {
            _animated.insert(track->targets().begin(), track->targets().end());
        }

        spdlog::info("Keys: 1 / 2 full body, 3 / 4 / 5 upper body, A blend type, M mask, [ ] blend, S skeleton");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_anim) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_1: _anim->baseLayer()->transition("Idle", 0.4f); break;
        case SDLK_2: _anim->baseLayer()->transition("Walk", 0.4f); break;
        case SDLK_3: _upperBody->transition("Eager", 0.4f); break;
        case SDLK_4: _upperBody->transition("Idle", 0.4f); break;
        case SDLK_5: _upperBody->transition("Dance", 0.4f); break;
        case SDLK_A:
            _upperBody->setBlendType(_upperBody->blendType() == AnimLayerBlendType::ADDITIVE
                ? AnimLayerBlendType::OVERWRITE : AnimLayerBlendType::ADDITIVE);
            break;
        case SDLK_M:
            _upperBody->setMaskSubtrees(_upperBody->maskSubtrees().empty()
                ? std::vector<std::string>{kUpperBodyRoot} : std::vector<std::string>{});
            break;
        case SDLK_LEFTBRACKET:
        case SDLK_RIGHTBRACKET:
            _blend = std::clamp(_blend + (event.key.key == SDLK_RIGHTBRACKET ? 0.1f : -0.1f), 0.0f, 1.0f);
            _anim->baseLayer()->setWeight(1.0f - _blend);
            _upperBody->setWeight(_blend);
            break;
        case SDLK_S: _skeleton = !_skeleton; break;
        default: return false;
        }
        spdlog::info("blend {:.1f}, upper body {} {}, skeleton {}", _blend,
            _upperBody->blendType() == AnimLayerBlendType::ADDITIVE ? "additive" : "overwrite",
            _upperBody->maskSubtrees().empty() ? "unmasked" : "masked", _skeleton);
        return true;
    }

    void preRender() override
    {
        // The skeleton where the animation put the bones this frame
        if (_skeleton && _model) {
            drawBones(_model, _model->name());
        }
    }

private:
    // A line from each animated node to each of its children, coloured by the weight of the
    // layers driving the node: red the base layer's, blue the upper body's. `path` is the node's
    // path from the model's root, as the clips name their targets.
    void drawBones(GraphNode* node, const std::string& path)
    {
        for (const auto& owned : node->children()) {
            GraphNode* child = owned.get();
            if (!child) {
                continue;
            }
            if (_animated.contains(path)) {
                const float baseWeight = _anim->baseLayer()->drives(path) ? _anim->baseLayer()->weight() : 0.0f;
                const float upperWeight = _upperBody->drives(path) ? _upperBody->weight() : 0.0f;
                scene()->drawLine(node->position(), child->position(), Color(baseWeight, 0.0f, upperWeight, 1.0f),
                    false);
            }
            drawBones(child, path + "/" + child->name());
        }
    }

    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _idleAsset;
    std::unique_ptr<Asset> _eagerAsset;
    std::unique_ptr<Asset> _walkAsset;
    std::unique_ptr<Asset> _danceAsset;
    std::unique_ptr<Asset> _helipad;
    Entity* _model = nullptr;
    AnimComponent* _anim = nullptr;
    AnimComponentLayer* _upperBody = nullptr;
    std::unordered_set<std::string> _animated;
    float _blend = 0.5f;
    bool _skeleton = true;
};

VISUTWIN_EXAMPLE_MAIN(LayerMasksExample)
