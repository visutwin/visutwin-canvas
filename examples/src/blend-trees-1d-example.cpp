// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream animation/blend-trees-1d.
//
// The bitmoji character driven by an anim state graph whose one "Movement" state is a 1D blend
// tree on the float parameter `blend`: Idle at 0 and the win dance at 1 (played at 0.85 speed).
// The blend starts at 0, all idle. Helipad sky (mip 2, exposure 2), a shadowed directional light
// (PCF5, distance 6), the camera 3 units in front at chest height.
//
// Keys stand in for upstream's slider (0 .. 1):
//   [ / ]  blend -/+ 0.1
//   0 / 1  all idle / all dance
//
#include <algorithm>
#include <memory>

#include "../animClips.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

class BlendTrees1dExample final : public ExampleApp
{
public:
    BlendTrees1dExample() : ExampleApp({.title = "Blend Trees 1D"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _idleAsset = std::make_unique<Asset>("idleAnim", AssetType::CONTAINER, assetPath("animations/bitmoji/idle.glb"));
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
        Entity* modelEntity = container ? container->instantiateRenderEntity() : nullptr;
        const auto idle = firstAnimTrack(_idleAsset.get());
        const auto dance = firstAnimTrack(_danceAsset.get());
        if (!modelEntity || !idle || !dance) {
            spdlog::error("blend-trees-1d needs bitmoji.glb and its idle and win-dance clips");
            return false;
        }
        modelEntity->setEngine(engine());

        // The state graph: START -> Movement, a 1D blend tree over `blend`
        AnimStateGraph stateGraph;
        stateGraph.addFloatParameter("blend", 0.0f);
        auto& layer = stateGraph.addLayer("characterState");
        AnimBlendTreeDesc blendTree;
        blendTree.type = AnimBlendType::BLEND_1D;
        blendTree.parameters = {"blend"};
        blendTree.children.push_back(AnimBlendTreeDesc{.name = "Idle", .point = Vector2(0.0f, 0.0f)});
        blendTree.children.push_back(AnimBlendTreeDesc{.name = "Dance", .point = Vector2(1.0f, 0.0f), .speed = 0.85f});
        layer.states.push_back(AnimStateDesc{"Movement", 1.0f, true, blendTree});
        layer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Movement"});

        _anim = static_cast<AnimComponent*>(modelEntity->addComponent<AnimComponent>());
        _anim->loadStateGraph(stateGraph);
        _anim->assignAnimation("Movement.Idle", idle);
        _anim->assignAnimation("Movement.Dance", dance);
        root()->addChild(modelEntity);

        spdlog::info("Keys: [ ] blend -/+ 0.1, 0 idle, 1 dance");
        return true;
    }

    void update(const float /*dt*/) override
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard || !_anim) {
            return;
        }
        float blend = _blend;
        if (keyboard->wasPressed(Key::LeftBracket)) blend -= 0.1f;
        if (keyboard->wasPressed(Key::RightBracket)) blend += 0.1f;
        if (keyboard->wasPressed(Key::Digit0)) blend = 0.0f;
        if (keyboard->wasPressed(Key::Digit1)) blend = 1.0f;
        blend = std::clamp(blend, 0.0f, 1.0f);
        if (blend != _blend) {
            _blend = blend;
            _anim->setFloat("blend", _blend);
            spdlog::info("blend {:.1f}", _blend);
        }
    }

private:
    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _idleAsset;
    std::unique_ptr<Asset> _danceAsset;
    std::unique_ptr<Asset> _helipad;
    AnimComponent* _anim = nullptr;
    float _blend = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(BlendTrees1dExample)
