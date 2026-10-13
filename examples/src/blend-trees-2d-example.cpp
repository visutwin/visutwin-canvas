// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Port of upstream's animation/blend-trees-2d-cartesian example: the bitmoji character
// driven by an anim state graph whose one "Emote" state is a 2D cartesian blend tree.
// Four clips sit on the plane of the parameters (posX, posY): Idle (-0.5, 0.5), Eager
// (0.5, 0.5), Walk (0.5, -0.5) and Dance (-0.5, -0.5); the blend point starts on Idle.
// The clips come from their own GLB files and bind to the character's rig by node name.
//
// Keys stand in for upstream's control panel, a pad the blend point is dragged across
// (x and y in [-1, 1]):
//   arrow keys  move the blend point by 0.1
//   1 - 4       put it on Idle, Eager, Walk or Dance
//   0           put it at the centre (an even four-way blend)
// DEVIATION: the panel's drawing of the point and of each clip's weight is not
// reproduced; the point is logged when it moves.
//
#include <algorithm>
#include <memory>
#include <string>

#include "../animClips.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

namespace
{
    struct BlendPoint
    {
        const char* name;
        float x;
        float y;
    };

    constexpr BlendPoint kPoints[] = {
        {"Idle", -0.5f, 0.5f}, {"Eager", 0.5f, 0.5f}, {"Walk", 0.5f, -0.5f}, {"Dance", -0.5f, -0.5f}};
}

class BlendTrees2dExample final: public ExampleApp
{
public:
    BlendTrees2dExample(): ExampleApp({.title = "Blend Trees 2D Cartesian"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        _modelAsset = std::make_unique<Asset>("model", AssetType::CONTAINER, assetPath("models/bitmoji.glb"));
        _idleAsset = std::make_unique<Asset>("idleAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/idle.glb"));
        _walkAsset = std::make_unique<Asset>("walkAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/walk.glb"));
        _eagerAsset = std::make_unique<Asset>("eagerAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/idle-eager.glb"));
        _danceAsset = std::make_unique<Asset>("danceAnim", AssetType::CONTAINER,
            assetPath("animations/bitmoji/win-dance.glb"));
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});

        // The sky dome.
        scene()->setExposure(2.0f);
        scene()->setSkyboxMip(2);
        if (Texture* helipad = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(helipad);
        }

        auto* cameraEntity = createCamera(Vector3(0.0f, 0.75f, 3.0f));
        if (auto* camera = cameraEntity->findComponent<CameraComponent>(); camera && camera->camera()) {
            camera->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        }

        auto* lightEntity = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f, true);
        if (auto* light = lightEntity->findComponent<LightComponent>()) {
            light->setShadowNormalBias(0.02f);
            light->setShadowType(ShadowType::SHADOW_PCF5_32F);
            light->setShadowDistance(6.0f);
            light->setShadowResolution(2048);
            light->setShadowBias(0.02f);
        }

        auto* container = _modelAsset->resourceAs<ContainerResource>();
        Entity* modelEntity = container ? container->instantiateRenderEntity() : nullptr;
        if (!modelEntity) {
            spdlog::error("bitmoji.glb failed to load");
            return false;
        }
        modelEntity->setName("model");
        modelEntity->setEngine(engine());

        const auto idle = firstAnimTrack(_idleAsset.get());
        const auto eager = firstAnimTrack(_eagerAsset.get());
        const auto dance = firstAnimTrack(_danceAsset.get());
        const auto walk = firstAnimTrack(_walkAsset.get());
        if (!idle || !eager || !dance || !walk) {
            return false;
        }

        AnimStateGraph stateGraph;
        stateGraph.addFloatParameter("posX", -0.5f);
        stateGraph.addFloatParameter("posY", 0.5f);
        auto& layer = stateGraph.addLayer("base");
        AnimBlendTreeDesc blendTree;
        blendTree.type = AnimBlendType::BLEND_2D_CARTESIAN;
        blendTree.parameters = {"posX", "posY"};
        for (const auto& point : kPoints) {
            blendTree.children.push_back(AnimBlendTreeDesc{.name = point.name, .point = Vector2(point.x, point.y)});
        }
        layer.states.push_back(AnimStateDesc{"Emote", 1.0f, true, blendTree});
        layer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Emote"});

        _anim = static_cast<AnimComponent*>(modelEntity->addComponent<AnimComponent>());
        _anim->loadStateGraph(stateGraph);
        _anim->assignAnimation("Emote.Idle", idle);
        _anim->assignAnimation("Emote.Eager", eager);
        _anim->assignAnimation("Emote.Dance", dance);
        _anim->assignAnimation("Emote.Walk", walk);

        root()->addChild(modelEntity);

        spdlog::info("Arrow keys move the blend point, 1-4 pick Idle / Eager / Walk / Dance, 0 the centre.");
        return true;
    }

    void update(const float /*dt*/) override
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard || !_anim) {
            return;
        }
        float x = _x;
        float y = _y;
        constexpr float kStep = 0.1f;
        if (keyboard->wasPressed(Key::Left)) x -= kStep;
        if (keyboard->wasPressed(Key::Right)) x += kStep;
        if (keyboard->wasPressed(Key::Down)) y -= kStep;
        if (keyboard->wasPressed(Key::Up)) y += kStep;
        constexpr Key kPointKeys[] = {Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4};
        for (size_t i = 0; i < std::size(kPointKeys); ++i) {
            if (keyboard->wasPressed(kPointKeys[i])) {
                x = kPoints[i].x;
                y = kPoints[i].y;
            }
        }
        if (keyboard->wasPressed(Key::Digit0)) {
            x = 0.0f;
            y = 0.0f;
        }
        x = std::clamp(x, -1.0f, 1.0f);
        y = std::clamp(y, -1.0f, 1.0f);
        if (x != _x || y != _y) {
            _x = x;
            _y = y;
            _anim->setFloat("posX", _x);
            _anim->setFloat("posY", _y);
            spdlog::info("blend point ({:.1f}, {:.1f})", _x, _y);
        }
    }

private:
    std::unique_ptr<Asset> _modelAsset;
    std::unique_ptr<Asset> _idleAsset;
    std::unique_ptr<Asset> _walkAsset;
    std::unique_ptr<Asset> _eagerAsset;
    std::unique_ptr<Asset> _danceAsset;
    std::unique_ptr<Asset> _helipad;

    AnimComponent* _anim = nullptr;
    float _x = -0.5f;
    float _y = 0.5f;
};

VISUTWIN_EXAMPLE_MAIN(BlendTrees2dExample)
