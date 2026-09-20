// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream test/contact-hardening-shadows.
//
// The animated robot-arm.glb (scale 3, idle clip at speed 0.1) stands on a large
// metallic plane under the dimmed helipad environment. Three PCSS shadow casters
// take turns every 5 seconds: a green "area" spot with a bright emissive plane
// showing its source, a white one-cascade directional light, and a red omni with a
// small emissive sphere. All three circle the arm. The camera orbits the arm at a
// distance of 25.
//
// Model credit: "Black Honey Robotic Arm" (Sketchfab), CC BY 4.0,
// https://sketchfab.com/3d-models/black-honey-robotic-arm-c50671f2a8e74de2a2e687103fdc93ab
//
// Keys stand in for upstream's control panel:
//   Space = cycle the active light on/off (upstream "Cycle Active Light")
//   A     = animate lights on/off
//   1/2/3 = toggle the area / point / directional light while not cycling
//   P     = switch all three lights between PCSS_32F and PCF3_32F
//
// DEVIATION: upstream's area light is a SPOT with LIGHTSHAPE_RECT, so it casts a
// PCSS shadow while lighting with an LTC rectangle. Here LIGHTTYPE_AREA_RECT is a
// separate positional type that casts no shadow, and a spot cannot take an area
// shape. Contact-hardening shadows are the point of this test, so the light stays
// a spot with the same cone, range, falloff and shadow settings and lights as a
// point source. The emissive plane still shows the rectangle.
// DEVIATION: upstream's PCF option is SHADOW_PCF5_32F, which this engine does not
// have. P switches to SHADOW_PCF3_32F.
// DEVIATION: the area spot's shadowBias is 0, where upstream leaves the default
// 0.05. Upstream's spot shader compares the stored depth with no bias of its own.
// This engine's non-clustered local-shadow shader subtracts shadowBias * 20 from
// the receiver depth, 0.01 at the default. With near 0.01 and range 150, the arm
// and the floor sit only about 0.0006 apart in depth, so every fragment passed
// and the spot cast no visible shadow under PCSS or PCF.
// DEVIATION: the directional penumbraSize is 0.02, where upstream uses 1. In this
// engine the directional penumbra is world-space, penumbraSize multiplied by the
// cascade's caster depth range. At 1 the shadow disappears, at 0.1 it is barely
// visible, and 0.02 comes closest to upstream's soft but visible shadow.
// DEVIATION: upstream scales the ground plane (100, 0, 100). This port uses a Y
// scale of 0.001, because the Metal normal matrix divides by the model
// determinant and a zero determinant zeroes the plane's normals.
// DEVIATION: the anim component is driven by a state graph, so upstream's
// assignAnimation('Idle', ...) becomes a one-state graph. Upstream sets the speed
// on the component (0.1), and this port does the same.
// DEVIATION: upstream loads the area-light LUT JSON and playcanvas-cube.glb. The
// LUTs are built into this engine, and upstream never uses the cube, so neither is
// loaded. upstream's ambientLuminance = 0 has no counterpart here: the ambient
// colour is already black.
// DEVIATION: upstream's orbit camera pivots on the arm's AABB centre, keeps the
// direction from its start position (0, 5, 11), and sets the distance to 25 on
// the first update. CameraControls is given the same pivot and distance.
//
#include <cmath>
#include <memory>
#include <string>
#include <variant>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/anim/state-graph/animStateGraph.h"
#include "framework/assets/asset.h"
#include "framework/components/anim/animComponent.h"
#include "framework/components/anim/animComponentSystem.h"
#include "framework/components/animation/animationComponent.h"
#include "framework/parsers/glbContainerResource.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kPi = 3.14159265358979f;
}

class PcssLocalExample final: public ExampleApp
{
public:
    PcssLocalExample(): ExampleApp({.title = "Contact Hardening Shadows"}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<AnimComponentSystem>();
    }

    bool create() override
    {
        // ------ Scene ------
        _helipadAsset = std::make_unique<Asset>(
            "helipad-env-atlas", AssetType::TEXTURE, assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        const auto helipadResource = _helipadAsset->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad env atlas");
            return false;
        }
        scene()->setSkyboxMip(1);
        scene()->setAmbientLight(0.0f, 0.0f, 0.0f);
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));
        scene()->setClusteredLightingEnabled(false);
        scene()->setSkyboxIntensity(0.1f);

        // ------ Ground plane ------
        _planeMaterial = std::make_shared<StandardMaterial>();
        _planeMaterial->setGloss(0.0f);
        _planeMaterial->setMetalness(0.7f);
        _planeMaterial->setUseMetalness(true);
        createPrimitive("plane", _planeMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(100.0f, 0.001f, 100.0f));

        // ------ Robot arm ------
        _armAsset = std::make_unique<Asset>("asset", AssetType::CONTAINER, assetPath("models/robot-arm.glb"));
        auto* armContainer = loadContainer(_armAsset);
        if (!armContainer) {
            return false;
        }
        _occluder = armContainer->instantiateRenderEntity();
        _occluder->setEngine(engine());
        if (auto* legacyAnim = _occluder->findComponent<AnimationComponent>()) {
            legacyAnim->setPlaying(false);
            legacyAnim->setEnabled(false);
        }
        if (!armContainer->animTracks().empty()) {
            AnimStateGraph stateGraph;
            auto& layer = stateGraph.addLayer("Base");
            layer.states.push_back(AnimStateDesc{"Idle"});
            layer.transitions.push_back(AnimTransitionDesc{.from = "START", .to = "Idle"});

            auto* anim = static_cast<AnimComponent*>(_occluder->addComponent<AnimComponent>());
            anim->setActivate(true);
            anim->loadStateGraph(stateGraph);
            anim->assignAnimation("Idle", armContainer->animTracks().begin()->second);
            anim->setSpeed(0.1f);
        } else {
            spdlog::warn("robot-arm.glb carries no animation");
        }
        _occluder->setLocalScale(3.0f, 3.0f, 3.0f);
        root()->addChild(_occluder);

        // ------ Area light (spot, see DEVIATION) ------
        _areaLight = new Entity();
        _areaLight->setEngine(engine());
        _area = static_cast<LightComponent*>(_areaLight->addComponent<LightComponent>());
        _area->setType(LightType::LIGHTTYPE_SPOT);
        _area->setColor(Color(0.25f, 1.0f, 0.25f, 1.0f));
        _area->setCastShadows(true);
        _area->setRange(150.0f);
        _area->setShadowResolution(2048);
        _area->setShadowDistance(100.0f);
        _area->setPenumbraSize(2.0f);
        _area->setShadowType(SHADOW_PCSS_32F);
        _area->setIntensity(16.0f);
        _area->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
        _area->setInnerConeAngle(45.0f);
        _area->setOuterConeAngle(50.0f);
        _area->setShadowNormalBias(0.1f);
        // Upstream leaves the default bias of 0.05. See the DEVIATION in the header.
        _area->setShadowBias(0.0f);
        _areaLight->setLocalScale(3.0f, 1.0f, 3.0f);
        _areaLight->setLocalEulerAngles(45.0f, 90.0f, 0.0f);
        _areaLight->setLocalPosition(4.0f, 7.0f, 0.0f);

        // Emissive material that is the light source colour. The unlit path adds
        // the base colour to the emissive, so the diffuse is black: with no lights
        // and no ambient, upstream's diffuse contributes nothing either.
        _brightMaterial = std::make_shared<StandardMaterial>();
        _brightMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _brightMaterial->setEmissive(_area->color());
        _brightMaterial->setEmissiveIntensity(_area->intensity());
        _brightMaterial->setUseLighting(false);
        _brightMaterial->setCullMode(CullMode::CULLFACE_NONE);
        addShape(_areaLight, "plane", _brightMaterial.get(), 1.0f);
        root()->addChild(_areaLight);

        // ------ Directional light ------
        _directionalLight = new Entity();
        _directionalLight->setEngine(engine());
        _directional = static_cast<LightComponent*>(_directionalLight->addComponent<LightComponent>());
        _directional->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        _directional->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _directional->setCastShadows(true);
        _directional->setNumCascades(1);
        // Upstream uses 1. See the DEVIATION in the header for why this is 0.02.
        _directional->setPenumbraSize(0.02f);
        _directional->setShadowType(SHADOW_PCSS_32F);
        _directional->setIntensity(2.0f);
        _directional->setShadowBias(0.5f);
        _directional->setShadowDistance(50.0f);
        _directional->setShadowNormalBias(0.1f);
        _directional->setShadowResolution(8192);
        _directionalLight->setLocalEulerAngles(65.0f, 35.0f, 0.0f);
        root()->addChild(_directionalLight);

        // ------ Omni light ------
        _omniLight = new Entity();
        _omniLight->setName("Omni");
        _omniLight->setEngine(engine());
        _omni = static_cast<LightComponent*>(_omniLight->addComponent<LightComponent>());
        _omni->setType(LightType::LIGHTTYPE_OMNI);
        _omni->setColor(Color(1.0f, 0.25f, 0.25f, 1.0f));
        _omni->setRange(25.0f);
        _omni->setPenumbraSize(2.0f);
        _omni->setShadowType(SHADOW_PCSS_32F);
        _omni->setIntensity(4.0f);
        _omni->setCastShadows(true);
        _omni->setShadowBias(0.2f);
        _omni->setShadowNormalBias(0.2f);
        _omni->setShadowResolution(2048);
        _omniLight->setLocalPosition(-4.0f, 7.0f, 0.0f);

        _omniMaterial = std::make_shared<StandardMaterial>();
        _omniMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _omniMaterial->setEmissive(_omni->color());
        _omniMaterial->setEmissiveIntensity(_omni->intensity());
        _omniMaterial->setUseLighting(false);
        _omniMaterial->setCullMode(CullMode::CULLFACE_NONE);
        addShape(_omniLight, "sphere", _omniMaterial.get(), 0.2f);
        root()->addChild(_omniLight);

        // ------ Camera ------
        auto* camera = createCamera(Vector3(0.0f, 5.0f, 11.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
            cameraComponent->setToneMapping(TONEMAP_ACES);
            cameraComponent->requestSceneColorMap(true);
        }
        const Vector3 pivot = entityBounds(_occluder).center();
        if (auto* controls = addOrbitControls(camera, pivot)) {
            controls->setOrbitDistance(25.0f);
            controls->storeResetState();
        }

        spdlog::info("Keys: Space = cycle, A = animate, 1/2/3 = area/point/directional, P = PCSS/PCF, Esc = quit");
        applyLightState();
        return true;
    }

    void update(const float dt) override
    {
        handleKeys();

        _timeDiff += dt;
        if (_cycle) {
            if (_timeDiff / 5.0f > 1.0f) {
                _index = (_index + 1) % 3;
                _timeDiff = 0.0f;
            }
        }
        applyLightState();

        if (_animate) {
            _time += dt;
            const float x = std::sin(_time * 0.2f);
            const float z = std::cos(_time * 0.2f);
            _omniLight->setLocalPosition(x * 4.0f, 5.0f, z * 4.0f);
            _directionalLight->setLocalEulerAngles(65.0f, 35.0f + _time * 2.0f, 0.0f);
            _areaLight->setLocalEulerAngles(45.0f, 180.0f + (_time * 0.2f * 180.0f) / kPi, 0.0f);
            _areaLight->setLocalPosition(-x * 4.0f, 7.0f, -z * 4.0f);
        }
    }

private:
    static GlbContainerResource* loadContainer(const std::unique_ptr<Asset>& asset)
    {
        const auto resource = asset->resource();
        if (!resource || !std::holds_alternative<ContainerResource*>(*resource)) {
            spdlog::error("GLB '{}' failed to load as a container", asset->name());
            return nullptr;
        }
        return dynamic_cast<GlbContainerResource*>(std::get<ContainerResource*>(*resource));
    }

    // Primitive shape that matches the light source shape. It casts no shadow.
    void addShape(Entity* parent, const char* type, Material* material, const float scale) const
    {
        auto* shape = new Entity();
        shape->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(shape->addComponent<RenderComponent>())) {
            render->setMaterial(material);
            render->setType(type);
            render->setCastShadows(false);
        }
        shape->setLocalScale(scale, scale, scale);
        parent->addChild(shape);
    }

    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        if (keyboard->wasPressed(Key::Space)) {
            _cycle = !_cycle;
            spdlog::info("Cycle active light: {}", _cycle ? "on" : "off");
        }
        if (keyboard->wasPressed(Key::A)) {
            _animate = !_animate;
            spdlog::info("Animate lights: {}", _animate ? "on" : "off");
        }
        if (keyboard->wasPressed(Key::Digit1)) _areaEnabled = !_areaEnabled;
        if (keyboard->wasPressed(Key::Digit2)) _pointEnabled = !_pointEnabled;
        if (keyboard->wasPressed(Key::Digit3)) _directionalEnabled = !_directionalEnabled;
        if (keyboard->wasPressed(Key::P)) {
            _pcss = !_pcss;
            const ShadowType type = _pcss ? SHADOW_PCSS_32F : SHADOW_PCF3_32F;
            _area->setShadowType(type);
            _omni->setShadowType(type);
            _directional->setShadowType(type);
            spdlog::info("Shadows: {}", _pcss ? "PCSS_32F" : "PCF3_32F");
        }
    }

    void applyLightState() const
    {
        if (_cycle) {
            _areaLight->setEnabled(_index == 0);
            _directionalLight->setEnabled(_index == 1);
            _omniLight->setEnabled(_index == 2);
        } else {
            _areaLight->setEnabled(_areaEnabled);
            _directionalLight->setEnabled(_directionalEnabled);
            _omniLight->setEnabled(_pointEnabled);
        }
    }

    std::unique_ptr<Asset> _helipadAsset;
    std::unique_ptr<Asset> _armAsset;

    std::shared_ptr<StandardMaterial> _planeMaterial;
    std::shared_ptr<StandardMaterial> _brightMaterial;
    std::shared_ptr<StandardMaterial> _omniMaterial;

    Entity* _occluder = nullptr;
    Entity* _areaLight = nullptr;
    Entity* _directionalLight = nullptr;
    Entity* _omniLight = nullptr;
    LightComponent* _area = nullptr;
    LightComponent* _directional = nullptr;
    LightComponent* _omni = nullptr;

    bool _cycle = true;
    bool _animate = true;
    bool _areaEnabled = true;
    bool _pointEnabled = true;
    bool _directionalEnabled = true;
    bool _pcss = true;

    float _time = 0.0f;
    float _timeDiff = 0.0f;
    int _index = 0;
};

VISUTWIN_EXAMPLE_MAIN(PcssLocalExample)
