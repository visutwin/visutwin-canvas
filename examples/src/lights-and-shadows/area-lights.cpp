// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.07.2026
//
// Port of upstream lights-and-shadows/area-lights.
//
// Area lights give a light source a shape and a size: a rectangle, a disk or a sphere. In a closed
// room (area-lights-room.glb, a little ambient light and no environment) the three glowing doors
// are rectangular spot lights, a drifting orb a spherical omni light, and the sun shining through
// the skylight a disk-shaped directional light, its shadow fitted to the room's bounds. Their
// reflections on the glossy floor and spheres take the shape of the source and blur with the
// roughness. Each light carries a visible emissive source for each shape (box, cylinder, sphere,
// and a small sphere for punctual), only the current one shown; the sun's is placed just inside
// the camera's far clip at the light's angular size. The statue, a glossy red sphere and a white
// ceramic one stand on the pedestals. The camera frame (4x MSAA, ACES) adds bloom and a vignette.
//
// Keys stand in for upstream's controls panel:
//   1 / 2 / 3    the doors / the orb / the sun on or off
//   Q / W / E    cycle the shape of the doors / the orb / the sun (rect, disk, sphere, punctual)
//   S            shadows on or off for every light
//   - / =        the floor's roughness -/+ 0.05 (0 - 1)
//   [ / ]        the red sphere's roughness -/+ 0.05
//   M            the red sphere metallic or not
//   B            bloom on or off
//   Space        pause the animation
//
// DEVIATIONS:
// - The LTC lookup tables are built into the engine (upstream loads them from a JSON asset).
// - The clustered shadow atlas has no scene-wide filter type (upstream sets PCF5 16-bit); its
//   filter is the engine's, at the same 4096 resolution.
// - The glTF materials are found from the instantiated meshes rather than the container's list.
// - Our glTF materials keep gloss as 1 - roughness (upstream's loader sets glossInvert and stores the
//   roughness), so the floor's roughness is written as 1 - roughness.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <vector>

#include "exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/render/renderComponent.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kEmissiveScale = 4.0f;   // the sources' brightness relative to their light
    constexpr std::array<LightShape, 4> kShapes = {LightShape::LIGHTSHAPE_RECT, LightShape::LIGHTSHAPE_DISK,
        LightShape::LIGHTSHAPE_SPHERE, LightShape::LIGHTSHAPE_PUNCTUAL};

    LightShape nextShape(const LightShape shape)
    {
        const auto it = std::find(kShapes.begin(), kShapes.end(), shape);
        return kShapes[(static_cast<size_t>(it - kShapes.begin()) + 1) % kShapes.size()];
    }

    // The rotation that turns `from` onto `to` (both unit length).
    Quaternion rotationBetween(const Vector3& from, const Vector3& to)
    {
        const float d = std::clamp(from.dot(to), -1.0f, 1.0f);
        Vector3 axis = from.cross(to);
        if (axis.length() < 1e-6f) {
            if (d > 0.0f) {
                return Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
            }
            axis = std::abs(from.getX()) < 0.9f ? Vector3(1.0f, 0.0f, 0.0f).cross(from) : Vector3(0.0f, 0.0f, 1.0f).cross(from);
        }
        return Quaternion::fromAxisAngle(axis.normalized(), std::acos(d) * 180.0f / std::numbers::pi_v<float>);
    }

    struct LightSettings
    {
        bool enabled = true;
        LightShape shape = LightShape::LIGHTSHAPE_RECT;
        float intensity = 1.0f;
        bool shadows = true;
    };
}

class AreaLightExample final : public ExampleApp
{
public:
    AreaLightExample() : ExampleApp({}) {}

protected:
    bool create() override
    {
        _room = std::make_unique<Asset>("room", AssetType::CONTAINER, assetPath("models/area-lights-room.glb"));
        _statue = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        auto* roomContainer = _room->resourceAs<ContainerResource>();
        auto* statueContainer = _statue->resourceAs<ContainerResource>();
        if (!roomContainer || !statueContainer) {
            spdlog::error("area-lights: a model did not load");
            return false;
        }

        // The omni and spot lights are clustered, which supports area lights once enabled.
        auto& lighting = scene()->lighting();
        lighting.areaLightsEnabled = true;
        lighting.shadowsEnabled = true;
        lighting.shadowAtlasResolution = 4096;

        // The room is closed: no environment lighting, only a little ambient light.
        scene()->setAmbientLight(0.03f, 0.032f, 0.04f);

        // The room: floor plates, ribbed walls with three door recesses, a ceiling with a skylight.
        Entity* room = roomContainer->instantiateRenderEntity();
        room->setEngine(engine());
        root()->addChild(room);
        Entity* statue = statueContainer->instantiateRenderEntity();
        statue->setEngine(engine());
        statue->setLocalScale(0.12f, 0.12f, 0.12f);
        statue->setLocalPosition(-4.0f, 0.55f, 0.8f);
        statue->setLocalEulerAngles(0.0f, 25.0f, 0.0f);
        root()->addChild(statue);
        for (Entity* model : {room, statue}) {
            for (auto* render : model->findComponents<RenderComponent>()) {
                render->setCastShadows(true);
                render->setReceiveShadows(true);
                for (auto* meshInstance : render->meshInstances()) {
                    auto* material = dynamic_cast<StandardMaterial*>(meshInstance ? meshInstance->material() : nullptr);
                    if (material && material->name() == "Floor") {
                        _floorMaterial = material;
                    }
                }
            }
        }
        if (!_floorMaterial) {
            spdlog::error("area-lights: the room has no 'Floor' material");
            return false;
        }

        // A glossy red sphere and a white ceramic one on the pedestals.
        _redMaterial = createSphere(Vector3(0.0f, 1.26f, 0.0f), 0.9f, Color(0.7f, 0.02f, 0.02f, 1.0f));
        _ceramicMaterial = createSphere(Vector3(4.0f, 1.15f, 0.8f), 0.6f, Color(0.9f, 0.9f, 0.88f, 1.0f));
        _ceramicMaterial->setGloss(0.3f);

        // The camera at the front of the room, looking at the doors; the sky shows through the
        // skylight as the clear colour.
        _camera = createCamera(Vector3(1.3f, 2.5f, 6.8f));
        _cameraComponent = _camera->findComponent<CameraComponent>();
        Camera* camera = _cameraComponent->camera();
        camera->setClearColor(Color(0.45f, 0.6f, 0.8f, 1.0f));
        camera->setFov(60.0f);
        camera->setNearClip(0.1f);
        camera->setFarClip(1000.0f);
        _cameraComponent->setToneMapping(TONEMAP_ACES);
        if (auto* controls = addOrbitControls(_camera, Vector3(0.0f, 1.5f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-38.0f, 8.0f));
            controls->setZoomRange(Vector2(2.0f, 7.0f));
            controls->storeResetState();
        }
        auto rendering = _cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomBlurLevel = 6;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.35f;
        _cameraComponent->setRendering(rendering);

        // The glowing doors: spot lights filling the door recesses. A spot shines down its -Y
        // axis, turned to face the room, its height along Z.
        for (const float x : {-4.5f, 0.0f, 4.5f}) {
            LightRig rig = createLightRig(LightType::LIGHTTYPE_SPOT, false);
            if (auto* light = rig.light) {
                light->setRange(25.0f);
                light->setInnerConeAngle(60.0f);
                light->setOuterConeAngle(80.0f);
                light->setShadowBias(0.2f);
                light->setShadowNormalBias(0.05f);
            }
            rig.entity->setLocalPosition(x, 1.7f, -9.45f);
            rig.entity->setLocalEulerAngles(-90.0f, 0.0f, 0.0f);
            _doors.push_back(rig);
        }

        // A glowing orb: an omni light drifting around the room.
        _orb = createLightRig(LightType::LIGHTTYPE_OMNI, false);
        if (auto* light = _orb.light) {
            light->setRange(15.0f);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.05f);
        }

        // The sun: a directional light through the skylight, its shadow covering just the room.
        // Its area is sized at the camera's far clip, so its sources are placed separately, just
        // inside it.
        _sun = createLightRig(LightType::LIGHTTYPE_DIRECTIONAL, true);
        if (auto* light = _sun.light) {
            light->setShadowType(SHADOW_PCF3_32F);
            light->setShadowResolution(2048);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.02f);
            light->setShadowBounds(BoundingBox(Vector3(0.0f, 3.4f, 0.0f), Vector3(8.6f, 3.4f, 9.6f)));
        }
        _sunDistance = camera->farClip() * 0.9f;

        _doorSettings = {true, LightShape::LIGHTSHAPE_RECT, 3.0f, true};
        _orbSettings = {true, LightShape::LIGHTSHAPE_SPHERE, 6.0f, true};
        _sunSettings = {true, LightShape::LIGHTSHAPE_DISK, 2.5f, true};
        applySettings();
        update(0.0f);

        spdlog::info("area-lights: 1 2 3 lights, Q W E shapes, S shadows, - = floor, [ ] sphere roughness, "
                     "M metal, B bloom, Space pause");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_1: _doorSettings.enabled = !_doorSettings.enabled; break;
        case SDLK_2: _orbSettings.enabled = !_orbSettings.enabled; break;
        case SDLK_3: _sunSettings.enabled = !_sunSettings.enabled; break;
        case SDLK_Q: _doorSettings.shape = nextShape(_doorSettings.shape); break;
        case SDLK_W: _orbSettings.shape = nextShape(_orbSettings.shape); break;
        case SDLK_E: _sunSettings.shape = nextShape(_sunSettings.shape); break;
        case SDLK_S: {
            const bool shadows = !_doorSettings.shadows;
            _doorSettings.shadows = _orbSettings.shadows = _sunSettings.shadows = shadows;
            break;
        }
        case SDLK_MINUS: _floorRoughness = std::max(0.0f, _floorRoughness - 0.05f); break;
        case SDLK_EQUALS: _floorRoughness = std::min(1.0f, _floorRoughness + 0.05f); break;
        case SDLK_LEFTBRACKET: _sphereRoughness = std::max(0.0f, _sphereRoughness - 0.05f); break;
        case SDLK_RIGHTBRACKET: _sphereRoughness = std::min(1.0f, _sphereRoughness + 0.05f); break;
        case SDLK_M: _sphereMetalness = _sphereMetalness > 0.5f ? 0.0f : 1.0f; break;
        case SDLK_B: _bloom = !_bloom; break;
        case SDLK_SPACE: _animate = !_animate; break;
        default: return false;
        }
        applySettings();
        return true;
    }

    void update(const float dt) override
    {
        if (_animate) {
            _time += dt;
        }

        // The orb circles the pedestals, bobbing up and down.
        const float orbAngle = _time * 0.4f;
        _orb.entity->setPosition(std::sin(orbAngle) * 5.5f, 2.2f + std::sin(_time * 1.1f) * 0.6f,
            0.5f + std::cos(orbAngle) * 3.2f);

        // The sun shines down at its elevation, from its azimuth.
        constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;
        const float elevation = 68.0f * kDegToRad;
        const float azimuth = 200.0f * kDegToRad;
        const Vector3 direction(-std::cos(elevation) * std::sin(azimuth), -std::sin(elevation),
            -std::cos(elevation) * std::cos(azimuth));
        const Quaternion rotation = rotationBetween(Vector3(0.0f, -1.0f, 0.0f), direction);
        _sun.entity->setRotation(rotation);

        // The visible sun, just inside the far clip, keeps the angular size of its light.
        const Vector3 position = direction * -_sunDistance + _camera->position();
        _sun.sourcesParent->setPosition(position);
        _sun.sourcesParent->setRotation(rotation);
        const float sourceScale = _sunScale * _sunDistance;
        _sun.sourcesParent->setLocalScale(sourceScale, sourceScale, sourceScale);
    }

private:
    struct LightRig
    {
        Entity* entity = nullptr;          // the light, scaled to size the area light
        LightComponent* light = nullptr;
        Entity* sourcesParent = nullptr;   // the visible sources' parent
        std::map<LightShape, Entity*> sources;
        std::shared_ptr<StandardMaterial> material;
    };

    std::shared_ptr<StandardMaterial> createSphere(const Vector3& position, const float radius, const Color& color)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setUseMetalness(true);
        material->setMetalness(0.0f);
        // The gloss is the roughness, as for the room's materials.
        material->setGlossInvert(true);
        _materials.push_back(material);
        Entity* sphere = createPrimitive("sphere", material.get(), position,
            Vector3(radius * 2.0f, radius * 2.0f, radius * 2.0f));
        sphere->setName("Sphere");
        return material;
    }

    // A light with a visible source for each shape, sized to match a light of the same scale; the
    // rectangle and the disk lie in the light's XZ plane, the sphere is centred on it.
    LightRig createLightRig(const LightType type, const bool separateSources)
    {
        LightRig rig;
        rig.entity = new Entity();
        rig.entity->setEngine(engine());
        rig.light = static_cast<LightComponent*>(rig.entity->addComponent<LightComponent>());
        if (rig.light) {
            rig.light->setType(type);
            rig.light->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
        }
        root()->addChild(rig.entity);

        rig.material = std::make_shared<StandardMaterial>();
        rig.material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        rig.material->setUseLighting(false);

        rig.sourcesParent = rig.entity;
        if (separateSources) {
            rig.sourcesParent = new Entity();
            rig.sourcesParent->setEngine(engine());
            root()->addChild(rig.sourcesParent);
        }
        const auto addSource = [&](const LightShape shape, const char* primitive, const Vector3& scale) {
            // Not a shadow caster, or it would block its own light.
            Entity* source = createPrimitive(primitive, rig.material.get());
            if (auto* render = source->findComponent<RenderComponent>()) {
                render->setCastShadows(false);
            }
            rig.sourcesParent->addChild(source);
            source->setLocalPosition(0.0f, 0.0f, 0.0f);
            source->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
            source->setEnabled(false);
            rig.sources[shape] = source;
        };
        addSource(LightShape::LIGHTSHAPE_RECT, "box", Vector3(1.0f, 0.002f, 1.0f));
        addSource(LightShape::LIGHTSHAPE_DISK, "cylinder", Vector3(1.0f, 0.002f, 1.0f));
        addSource(LightShape::LIGHTSHAPE_SPHERE, "sphere", Vector3(1.0f, 1.0f, 1.0f));
        addSource(LightShape::LIGHTSHAPE_PUNCTUAL, "sphere", Vector3(0.1f, 0.1f, 0.1f));
        return rig;
    }

    static void applyLightSettings(LightRig& rig, const LightSettings& settings, const Color& color,
        const float emissiveIntensity)
    {
        rig.entity->setEnabled(settings.enabled);
        rig.sourcesParent->setEnabled(settings.enabled);
        if (rig.light) {
            rig.light->setShape(settings.shape);
            rig.light->setColor(color);
            rig.light->setIntensity(settings.intensity);
            rig.light->setCastShadows(settings.shadows);
        }
        rig.material->setEmissive(color);
        rig.material->setEmissiveIntensity(emissiveIntensity);
        for (auto& [shape, source] : rig.sources) {
            source->setEnabled(shape == settings.shape);
        }
    }

    // A light scaled to the size of its area: the rectangle and the disk along X and Z, the
    // sphere by X.
    static void setLightSize(Entity* entity, const LightShape shape, const float width, const float height)
    {
        if (shape == LightShape::LIGHTSHAPE_PUNCTUAL) {
            entity->setLocalScale(1.0f, 1.0f, 1.0f);
        } else if (shape == LightShape::LIGHTSHAPE_SPHERE) {
            entity->setLocalScale(width, width, width);
        } else {
            entity->setLocalScale(width, 1.0f, height);
        }
    }

    void applySettings()
    {
        const std::array<Color, 3> doorColors = {Color(1.0f, 0.42f, 0.12f, 1.0f), Color(0.25f, 0.8f, 1.0f, 1.0f),
            Color(1.0f, 0.8f, 0.35f, 1.0f)};
        for (size_t i = 0; i < _doors.size(); ++i) {
            applyLightSettings(_doors[i], _doorSettings, doorColors[i], _doorSettings.intensity * kEmissiveScale);
            setLightSize(_doors[i].entity, _doorSettings.shape, 1.7f, 3.3f);
        }
        const Color orbColor(0.8f, 0.88f, 1.0f, 1.0f);
        applyLightSettings(_orb, _orbSettings, orbColor, _orbSettings.intensity * kEmissiveScale);
        setLightSize(_orb.entity, _orbSettings.shape, 1.0f, 1.0f);

        // The directional light's area is sized relative to the far clip, so its scale is the
        // sun's angular size (8 degrees).
        const Color sunColor(1.0f, 0.86f, 0.68f, 1.0f);
        applyLightSettings(_sun, _sunSettings, sunColor, _sunSettings.intensity * kEmissiveScale);
        _sunScale = 2.0f * std::tan(8.0f * 0.5f * std::numbers::pi_v<float> / 180.0f);
        _sun.entity->setLocalScale(_sunScale, _sunScale, _sunScale);

        _floorMaterial->setGloss(1.0f - _floorRoughness);   // a glTF material: gloss = 1 - roughness
        _redMaterial->setGloss(_sphereRoughness);
        _redMaterial->setMetalness(_sphereMetalness);

        auto rendering = _cameraComponent->rendering();
        rendering.bloomIntensity = _bloom ? 0.03f : 0.0f;
        _cameraComponent->setRendering(rendering);
    }

    std::unique_ptr<Asset> _room;
    std::unique_ptr<Asset> _statue;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    StandardMaterial* _floorMaterial = nullptr;
    std::shared_ptr<StandardMaterial> _redMaterial;
    std::shared_ptr<StandardMaterial> _ceramicMaterial;
    Entity* _camera = nullptr;
    CameraComponent* _cameraComponent = nullptr;

    std::vector<LightRig> _doors;
    LightRig _orb;
    LightRig _sun;
    LightSettings _doorSettings;
    LightSettings _orbSettings;
    LightSettings _sunSettings;
    float _sunScale = 1.0f;
    float _sunDistance = 900.0f;

    float _floorRoughness = 0.4f;
    float _sphereRoughness = 0.15f;
    float _sphereMetalness = 0.0f;
    bool _bloom = true;
    bool _animate = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(AreaLightExample)
