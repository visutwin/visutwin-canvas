// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream animation/component-properties.
//
// The anim component animating the properties of OTHER components. Two spot lights (range 15,
// cones 5 and 10 degrees, black to begin with) hang 10 units above a box (half a unit, the grey
// logo texture) on a 15 x 15 plane, children of an entity "lights" that carries the anim component.
// Two clips drive both lights' colour (a light component property) and their rotation (the
// entity's localEulerAngles), by paths from the anim component's entity:
//   Static         both green and pointing straight down
//   Flash          both flashing red (full and 0.4, every half second) while they sweep in
//                  opposite directions: tilted 4 degrees about X (one minus 4) and turning 180
//                  degrees about Y and back over two seconds
// The scene starts in Static; the camera at (7, 10, 7) looks at the origin and clears to black.
//
// Keys standing in for upstream's controls panel:
//   F  switch between Static and Flash
//
// DEVIATIONS:
// - The clips are built as AnimTracks here, where upstream opens its clip data through an
//   AnimClipHandler; their keys, curves and interpolation are upstream's.
// - The scene's lights are clustered (the engine's default), as upstream's.
//
#include <memory>
#include <string>
#include <vector>

#include "exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentLayer.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/anim/evaluator/animTrack.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // A curve of `track` driving `property` of `component` on the entity at `path`.
    void addCurve(AnimTrack& track, const std::string& path, const std::string& component,
        const std::string& property, const size_t input, const size_t output)
    {
        AnimCurve curve;
        curve.nodeName = path;
        curve.component = component;
        curve.propertyPath = property;
        curve.inputIndex = input;
        curve.outputIndex = output;
        curve.interpolation = AnimInterpolation::LINEAR;
        track.addCurve(curve);
    }

    AnimData data(const int components, std::vector<float> values)
    {
        AnimData result;
        result.components = components;
        result.data = std::move(values);
        return result;
    }

    // Two static spot lights: a green colour and no rotation
    std::shared_ptr<AnimTrack> staticLightClip()
    {
        auto track = std::make_shared<AnimTrack>("staticLight", 1.0f);
        track->addInput(data(1, {0.0f}));
        track->addOutput(data(4, {0.0f, 1.0f, 0.0f, 1.0f}));
        track->addOutput(data(4, {0.0f, 0.0f, 0.0f, 0.0f}));
        addCurve(*track, "lights/spotLight1", "light", "color", 0, 0);
        addCurve(*track, "lights/spotLight2", "light", "color", 0, 0);
        addCurve(*track, "lights/spotLight1", "entity", "localEulerAngles", 0, 1);
        addCurve(*track, "lights/spotLight2", "entity", "localEulerAngles", 0, 1);
        return track;
    }

    // Two flashing spot lights: a red colour flashing every half second, and a sweep each way
    std::shared_ptr<AnimTrack> flashingLightClip()
    {
        auto track = std::make_shared<AnimTrack>("flashingLight", 2.0f);
        track->addInput(data(1, {0.0f, 0.5f, 1.0f, 1.5f, 2.0f}));
        track->addInput(data(1, {0.0f, 1.0f, 2.0f}));
        track->addOutput(data(4, {1.0f, 0.0f, 0.0f, 1.0f, 0.4f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
            0.4f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f}));
        track->addOutput(data(4, {4.0f, 0.0f, 0.0f, 0.0f, 4.0f, 180.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f}));
        track->addOutput(data(4, {-4.0f, 0.0f, 0.0f, 0.0f, -4.0f, 180.0f, 0.0f, 0.0f, -4.0f, 0.0f, 0.0f, 0.0f}));
        addCurve(*track, "lights/spotLight1", "light", "color", 0, 0);
        addCurve(*track, "lights/spotLight2", "light", "color", 0, 0);
        addCurve(*track, "lights/spotLight1", "entity", "localEulerAngles", 1, 1);
        addCurve(*track, "lights/spotLight2", "entity", "localEulerAngles", 1, 2);
        return track;
    }
}

class ComponentPropertiesExample final : public ExampleApp
{
public:
    ComponentPropertiesExample() : ExampleApp({}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        _textureAsset = std::make_unique<Asset>("playcanvasGreyTexture", AssetType::TEXTURE,
            assetPath("textures/playcanvas-grey.png"));
        Texture* texture = _textureAsset->resourceAs<Texture>();
        if (!texture) {
            spdlog::error("component-properties needs textures/playcanvas-grey.png");
            return false;
        }

        Entity* camera = createCamera(Vector3(7.0f, 10.0f, 7.0f));
        camera->setName("camera");
        camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));

        _boxMaterial = std::make_shared<StandardMaterial>();
        _boxMaterial->setDiffuseMap(texture);
        Entity* box = createPrimitive("box", _boxMaterial.get(), Vector3(0.0f, 0.25f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
        box->setName("model");
        _planeMaterial = std::make_shared<StandardMaterial>();
        Entity* plane = createPrimitive("plane", _planeMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(15.0f, 1.0f, 15.0f));
        plane->setName("plane");

        // The animatable lights, under the entity carrying the anim component
        auto* lights = new Entity();
        lights->setName("lights");
        lights->setEngine(engine());
        for (const char* name : {"spotLight1", "spotLight2"}) {
            auto* light = new Entity();
            light->setName(name);
            light->setEngine(engine());
            auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>());
            component->setType(LightType::LIGHTTYPE_SPOT);
            component->setColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
            component->setIntensity(1.0f);
            component->setRange(15.0f);
            component->setInnerConeAngle(5.0f);
            component->setOuterConeAngle(10.0f);
            light->setLocalPosition(0.0f, 10.0f, 0.0f);
            lights->addChild(light);
        }
        root()->addChild(lights);

        // The anim component on the lights entity, a clip for each state
        _anim = static_cast<AnimComponent*>(lights->addComponent<AnimComponent>());
        _anim->setSpeed(1.0f);
        _anim->assignAnimation("Static", staticLightClip());
        _anim->assignAnimation("Flash", flashingLightClip());

        spdlog::info("Keys: F switches between Static and Flash");
        return true;
    }

    void update(float /*dt*/) override
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard || !keyboard->wasPressed(Key::F) || !_anim || !_anim->baseLayer()) {
            return;
        }
        AnimComponentLayer* layer = _anim->baseLayer();
        layer->transition(layer->activeState() == "Static" ? "Flash" : "Static");
        spdlog::info("state: {}", layer->activeState());
    }

private:
    std::unique_ptr<Asset> _textureAsset;
    std::shared_ptr<StandardMaterial> _boxMaterial;
    std::shared_ptr<StandardMaterial> _planeMaterial;
    AnimComponent* _anim = nullptr;
};

VISUTWIN_EXAMPLE_MAIN(ComponentPropertiesExample)
