// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
// Port of upstream camera-frame/lens-flare.
//
// The lens flare effect of a camera frame: a LensFlare follows a light. The sun of the procedural
// sky (low above the horizon, elevation 6, azimuth 160) gets a large flare with a diffraction
// burst and a train of ghosts, three lamps on poles (omni lights with emissive bulbs) a small
// glow, and two stage spot lights on masts flare only while the camera is inside their cone; the
// first sweeps slowly, so its cone crosses the camera from time to time. The flares fade as the
// walls cover their light, measured against the scene depth. The shapes of all flares are 15
// textures of one shape library, which the effect copies into a texture array. The camera frame
// renders with 4x MSAA, neutral tone mapping and a bloom of 0.02 over 6 levels.
//
// Keys stand in for upstream's controls panel:
//   [ / ]  flare intensity -/+ 0.05, 0..3 (0.25 to begin with)
//   - / =  flare resolution scale -/+ 0.05, 0.25..1 (0.5)
//   O      occlusion radius, cycled 0.01 / 0.02 / 0.04 / 0.08 (the sun's is 1.5x)
//   F      occlusion fade, cycled 0 / 0.15 / 0.5 / 1 s
//   E      edge fade, cycled 0 / 0.2 / 0.4 / 0.6
//   X      allow off screen on / off
//   1 2 3  the sun, the lamps, the spots on / off
//   V      the view, cycled: none, lens flare, bloom, scene
// VISUTWIN_LF_VIEW=name starts with that view, for a capture.
//
// DEVIATIONS:
// - upstream's orbitCamera script (inertia 0.2, distance 2..60) is replaced by the examples'
//   CameraControls in orbit mode, from the same start position. Its focus entity (at (0, 3, 0))
//   holds no meshes, so the script's bounds are empty and it orbits the ORIGIN, as this port does.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "extras/script/cameraControls.h"
#include "extras/script/proceduralSky.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "scene/constants.h"
#include "scene/graphics/lensFlareEffect.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // The lens flare shapes: procedurally generated textures (assets/textures/lens-flare), with
    // the shape in the colour channels and its coverage in alpha.
    constexpr std::array<const char*, 15> kShapeNames = {"glow_soft", "glow_disc", "glow_rim", "halo",
        "iris_hexagon", "iris_round", "iris_pentagon", "iris_triangle", "spike_6", "burst_star", "glint",
        "anamorphic_streak", "anamorphic_glow", "spectrum_arc", "spectrum_smudge"};

    constexpr std::array<const char*, 4> kViews = {"", "lensflare", "bloom", "scene"};

    LensFlareElement element(const char* shape, const float size, const Color& color = Color(1.0f, 1.0f, 1.0f, 1.0f),
        const float intensity = 1.0f)
    {
        LensFlareElement e;
        e.shape = shape;
        e.size = size;
        e.color = color;
        e.intensity = intensity;
        return e;
    }
}

class LensFlareExample final: public ExampleApp
{
public:
    // The scene is rendered to an antialiased texture, so the back buffer needs none.
    LensFlareExample(): ExampleApp({.title = "Lens Flare Example", .antialias = false}) {}

protected:
    bool create() override
    {
        _rocks = std::make_unique<Asset>("rocks", AssetType::TEXTURE, assetPath("textures/aerial_rocks_02_diff_1k.jpg"));
        for (const char* name : kShapeNames) {
            _shapeAssets.push_back(std::make_unique<Asset>(name, AssetType::TEXTURE,
                assetPath(std::string("textures/lens-flare/") + name + ".png")));
        }

        scene()->setExposure(1.0f);

        // The ground and a few walls, which cover the sun and the lamps as the camera orbits.
        _groundMaterial = createRockMaterial(12.0f);
        _wallMaterial = createRockMaterial(3.0f);
        createPrimitive("box", _groundMaterial.get(), Vector3(0.0f, -0.5f, 0.0f), Vector3(120.0f, 1.0f, 120.0f));
        createPrimitive("box", _wallMaterial.get(), Vector3(-6.0f, 3.0f, -8.0f), Vector3(1.0f, 6.0f, 10.0f));
        createPrimitive("box", _wallMaterial.get(), Vector3(7.0f, 2.5f, -3.0f), Vector3(6.0f, 5.0f, 1.0f));
        createPrimitive("box", _wallMaterial.get(), Vector3(0.0f, 4.0f, -14.0f), Vector3(12.0f, 8.0f, 1.0f));
        createPrimitive("box", _wallMaterial.get(), Vector3(10.0f, 3.5f, 8.0f), Vector3(1.5f, 7.0f, 1.5f));
        createPrimitive("box", _wallMaterial.get(), Vector3(-10.0f, 2.0f, 6.0f), Vector3(1.5f, 4.0f, 1.5f));

        // The sun: a directional light, which the procedural sky aims, colours and dims.
        _sun = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 2.5f, true);
        _sun->setName("Sun");
        if (auto* light = _sun->findComponent<LightComponent>()) {
            light->setShadowDistance(60.0f);
            light->setShadowResolution(2048);
            light->setShadowBias(0.3f);
            light->setShadowNormalBias(0.1f);
        }

        // The procedural sky, with the sun low above the horizon, in the view of the camera.
        auto* sky = new Entity();
        sky->setName("ProceduralSky");
        sky->setEngine(engine());
        sky->addComponent<ScriptComponent>();
        if (auto* script = sky->script()->create<ProceduralSky>()) {
            script->setSunLight(_sun);
            script->setElevation(6.0f);
            script->setAzimuth(160.0f);
            script->setTurbidity(4.0f);
            script->setMieCoefficient(0.002f);
            script->setRayleigh(2.5f);
            script->setLuminance(0.7f);
        }
        root()->addChild(sky);

        // Lamps: omni lights on poles, each with an emissive bulb.
        createLamp(Vector3(-4.0f, 3.5f, 2.0f), Color(1.0f, 0.6f, 0.2f, 1.0f));
        createLamp(Vector3(4.0f, 3.0f, 4.0f), Color(0.3f, 0.8f, 1.0f, 1.0f));
        createLamp(Vector3(1.0f, 4.0f, -6.0f), Color(1.0f, 0.3f, 0.7f, 1.0f));

        // Spot lights: stage lights on tall masts, aimed across the scene.
        createSpot(Vector3(-12.0f, 7.0f, -10.0f), Vector3(1.0f, -0.25f, 1.0f), Color(1.0f, 1.0f, 0.9f, 1.0f), 25.0f);
        createSpot(Vector3(14.0f, 6.0f, -2.0f), Vector3(-1.0f, -0.15f, 0.4f), Color(0.6f, 0.8f, 1.0f, 1.0f), 20.0f);

        // The camera, orbiting the origin (see the header).
        auto* camera = createCamera(Vector3(2.0f, 4.0f, 22.0f));
        _cameraComp = camera->findComponent<CameraComponent>();
        if (!_cameraComp || !_cameraComp->camera()) {
            return false;
        }
        _cameraComp->camera()->setFarClip(400.0f);
        _cameraComp->camera()->setFov(65.0f);
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setZoomRange(Vector2(2.0f, 60.0f));
            controls->storeResetState();
        }

        // The looks: the elements each kind of light places along the axis from the light through
        // the centre of the screen. Offsets of 0 sit on the light, 1 at the centre, 2 mirrored
        // across it; sizes are fractions of the screen height, and intensities are relative to the
        // light, as a flare is scaled by the intensity of its light.
        auto sunLook = std::make_shared<LensFlareLook>();
        {
            auto& e = sunLook->elements;
            e.push_back(element("glow_soft", 0.4f, Color(1.0f, 0.85f, 0.65f, 1.0f), 1.2f));
            e.push_back(element("burst_star", 0.8f, Color(1.0f, 0.95f, 0.85f, 1.0f), 1.4f));
            e.push_back(element("anamorphic_streak", 0.08f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.6f));
            e.back().aspect = 18.0f;
            e.push_back(element("halo", 0.5f, Color(1.0f, 0.9f, 0.8f, 1.0f), 0.2f));
            e.push_back(element("iris_hexagon", 0.05f, Color(0.5f, 0.8f, 1.0f, 1.0f), 0.14f));
            e.back().offset = 0.45f;
            e.back().count = 3;
            e.back().spread = 0.35f;
            e.push_back(element("spectrum_arc", 0.6f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.2f));
            e.back().offset = 1.1f;
            e.back().rotation = 180.0f;
            e.back().autoRotate = true;
            e.push_back(element("iris_round", 0.2f, Color(1.0f, 0.6f, 0.35f, 1.0f), 0.16f));
            e.back().offset = 1.35f;
            e.push_back(element("iris_pentagon", 0.1f, Color(0.6f, 1.0f, 0.6f, 1.0f), 0.24f));
            e.back().offset = 1.75f;
            e.push_back(element("glow_rim", 0.28f, Color(1.0f, 0.5f, 0.8f, 1.0f), 0.12f));
            e.back().offset = 2.2f;
            e.push_back(element("spectrum_smudge", 0.1f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.24f));
            e.back().offset = 2.6f;
            e.back().aspect = 2.5f;
            e.back().autoRotate = true;
        }
        auto lampLook = std::make_shared<LensFlareLook>();
        {
            auto& e = lampLook->elements;
            e.push_back(element("glow_disc", 0.14f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.4f));
            e.push_back(element("spike_6", 0.4f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.5f));
            e.push_back(element("iris_round", 0.035f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.15f));
            e.back().offset = 0.65f;
            e.back().count = 2;
            e.back().spread = 0.25f;
            e.push_back(element("iris_hexagon", 0.06f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.15f));
            e.back().offset = 1.5f;
        }
        auto spotLook = std::make_shared<LensFlareLook>();
        {
            auto& e = spotLook->elements;
            e.push_back(element("anamorphic_glow", 0.35f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.2f));
            e.back().aspect = 2.5f;
            e.push_back(element("anamorphic_streak", 0.06f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.17f));
            e.back().aspect = 22.0f;
            e.push_back(element("anamorphic_streak", 0.22f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.25f));
            e.back().aspect = 7.0f;
            e.push_back(element("glint", 0.5f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.07f));
            e.push_back(element("iris_triangle", 0.14f, Color(1.0f, 1.0f, 1.0f, 1.0f), 0.05f));
            e.back().offset = 1.6f;
        }

        // The effect, with the shape library and a flare per light.
        _lensFlare = std::make_shared<LensFlareEffect>();
        LensFlareShapeLibrary library;
        library.resolution = 512;
        for (const auto& asset : _shapeAssets) {
            library.shapes.emplace_back(asset->name(), asset->resourceAs<Texture>());
        }
        _lensFlare->setShapes(library);

        _sunFlare = std::make_shared<LensFlare>(sunLook, LensFlareSource{.light = _sun->findComponent<LightComponent>()});
        _lensFlare->add(_sunFlare);
        for (const auto& lamp : _lamps) {
            auto flare = std::make_shared<LensFlare>(lampLook, LensFlareSource{.light = lamp.light});
            // The bulb is half a unit across, so test the visibility in front of it.
            flare->occlusionOffset = 0.3f;
            flare->fadeStart = 30.0f;
            flare->fadeEnd = 50.0f;
            _lensFlare->add(flare);
            _flares.push_back(flare);
        }
        for (const auto& spot : _spots) {
            auto flare = std::make_shared<LensFlare>(spotLook, LensFlareSource{.light = spot.light});
            flare->occlusionOffset = 0.5f;
            _lensFlare->add(flare);
            _flares.push_back(flare);
        }
        _flares.insert(_flares.begin(), _sunFlare);

        // The camera frame.
        _cameraComp->setToneMapping(TONEMAP_NEUTRAL);
        auto rendering = _cameraComp->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.02f;
        rendering.bloomBlurLevel = 6;
        _cameraComp->setRendering(rendering);
        _cameraComp->addFrameEffect(_lensFlare);

        if (const char* view = std::getenv("VISUTWIN_LF_VIEW")) {
            for (size_t i = 0; i < kViews.size(); ++i) {
                if (std::string(kViews[i]) == view) {
                    _view = static_cast<int>(i);
                }
            }
        }
        applySettings();
        spdlog::info("Controls: [ ] intensity, -/= scale, O occlusion radius, F occlusion fade, E edge fade, "
                     "X off screen, 1 2 3 sun / lamps / spots, V view");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_lensFlare) {
            return false;
        }
        const auto cycle = [](const float value, const std::vector<float>& values) {
            for (size_t i = 0; i < values.size(); ++i) {
                if (value < values[i] - 1e-4f) {
                    return values[i];
                }
            }
            return values.front();
        };
        switch (event.key.key) {
        case SDLK_LEFTBRACKET: _intensity = std::max(0.0f, _intensity - 0.05f); break;
        case SDLK_RIGHTBRACKET: _intensity = std::min(3.0f, _intensity + 0.05f); break;
        case SDLK_MINUS: _scale = std::max(0.25f, _scale - 0.05f); break;
        case SDLK_EQUALS: _scale = std::min(1.0f, _scale + 0.05f); break;
        case SDLK_O: _occlusionRadius = cycle(_occlusionRadius, {0.01f, 0.02f, 0.04f, 0.08f}); break;
        case SDLK_F: _occlusionFade = cycle(_occlusionFade, {0.0f, 0.15f, 0.5f, 1.0f}); break;
        case SDLK_E: _edgeFade = cycle(_edgeFade, {0.0f, 0.2f, 0.4f, 0.6f}); break;
        case SDLK_X: _allowOffScreen = !_allowOffScreen; break;
        case SDLK_1: _sunOn = !_sunOn; break;
        case SDLK_2: _lampsOn = !_lampsOn; break;
        case SDLK_3: _spotsOn = !_spotsOn; break;
        case SDLK_V: _view = (_view + 1) % static_cast<int>(kViews.size()); break;
        default: return false;
        }
        applySettings();
        spdlog::info("intensity {:.2f}, scale {:.2f}, occlusion radius {:.3f}, fade {:.2f}, edge fade {:.2f}, "
                     "off screen {}, sun {}, lamps {}, spots {}, view {}", _intensity, _scale, _occlusionRadius,
            _occlusionFade, _edgeFade, _allowOffScreen, _sunOn, _lampsOn, _spotsOn,
            _view == 0 ? "none" : kViews[static_cast<size_t>(_view)]);
        return true;
    }

    void update(const float dt) override
    {
        // Sweep the first spot light, so that its cone crosses the camera from time to time.
        _sweep += dt * 18.0f;
        if (!_spots.empty()) {
            _spots.front().pivot->setLocalEulerAngles(0.0f, std::sin(_sweep * 0.02f) * 60.0f, 0.0f);
        }
    }

private:
    struct Lamp
    {
        LightComponent* light = nullptr;
        Entity* bulb = nullptr;
    };
    struct Spot
    {
        LightComponent* light = nullptr;
        Entity* pivot = nullptr;
    };

    std::shared_ptr<StandardMaterial> createRockMaterial(const float tiling)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuseMap(_rocks->resourceAs<Texture>());
        material->setDiffuseMapTiling(Vector2(tiling, tiling));
        material->setGloss(0.3f);
        return material;
    }

    // An emissive material of a colour, bright enough to bloom.
    std::shared_ptr<StandardMaterial> createEmissiveMaterial(const Color& color, const float intensity)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        material->setEmissive(color);
        material->setEmissiveIntensity(intensity);
        _materials.push_back(material);
        return material;
    }

    Entity* createLightEntity(const LightType type, const Color& color, const float intensity, const float range,
        LightComponent** out)
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        auto* light = static_cast<LightComponent*>(entity->addComponent<LightComponent>());
        light->setType(type);
        light->setColor(color);
        light->setIntensity(intensity);
        light->setRange(range);
        light->setCastShadows(false);
        *out = light;
        return entity;
    }

    void createLamp(const Vector3& position, const Color& color)
    {
        createPrimitive("cylinder", _wallMaterial.get(), Vector3(position.getX(), position.getY() * 0.5f, position.getZ()),
            Vector3(0.15f, position.getY(), 0.15f));
        Entity* bulb = createPrimitive("sphere", createEmissiveMaterial(color, 12.0f).get(), position,
            Vector3(0.5f, 0.5f, 0.5f));
        LightComponent* light = nullptr;
        Entity* lamp = createLightEntity(LightType::LIGHTTYPE_OMNI, color, 2.0f, 14.0f, &light);
        lamp->setName("Lamp");
        lamp->setLocalPosition(position);
        root()->addChild(lamp);
        _lamps.push_back({light, bulb});
    }

    void createSpot(const Vector3& position, const Vector3& direction, const Color& color, const float outerAngle)
    {
        createPrimitive("cylinder", _wallMaterial.get(), Vector3(position.getX(), position.getY() * 0.5f, position.getZ()),
            Vector3(0.2f, position.getY(), 0.2f));
        auto* pivot = new Entity();
        pivot->setName("SpotPivot");
        pivot->setEngine(engine());
        pivot->setLocalPosition(position);
        root()->addChild(pivot);

        LightComponent* light = nullptr;
        Entity* spot = createLightEntity(LightType::LIGHTTYPE_SPOT, color, 6.0f, 40.0f, &light);
        spot->setName("Spot");
        light->setInnerConeAngle(outerAngle * 0.6f);
        light->setOuterConeAngle(outerAngle);
        pivot->addChild(spot);
        // A light shines along the negative y axis of its entity: aim it along the direction.
        spot->lookAt(spot->position() + direction);
        spot->rotateLocal(90.0f, 0.0f, 0.0f);

        // The fixture: a short cylinder along the beam with an emissive face.
        auto* fixture = new Entity();
        fixture->setEngine(engine());
        auto* fixtureRender = static_cast<RenderComponent*>(fixture->addComponent<RenderComponent>());
        fixtureRender->setMaterial(_wallMaterial.get());
        fixtureRender->setType("cylinder");
        fixture->setLocalScale(0.6f, 0.6f, 0.6f);
        spot->addChild(fixture);
        auto* face = new Entity();
        face->setEngine(engine());
        auto faceMaterial = createEmissiveMaterial(color, 8.0f);
        auto* faceRender = static_cast<RenderComponent*>(face->addComponent<RenderComponent>());
        faceRender->setMaterial(faceMaterial.get());
        faceRender->setType("cylinder");
        face->setLocalPosition(0.0f, -0.3f, 0.0f);
        face->setLocalScale(0.5f, 0.05f, 0.5f);
        spot->addChild(face);

        _spots.push_back({light, pivot});
    }

    void applySettings()
    {
        _lensFlare->intensity = _intensity;
        _lensFlare->scale = _scale;
        for (const auto& flare : _flares) {
            flare->occlusionRadius = flare == _sunFlare ? _occlusionRadius * 1.5f : _occlusionRadius;
            flare->occlusionFade = _occlusionFade;
            flare->edgeFade = _edgeFade;
            flare->allowOffScreen = _allowOffScreen;
        }
        if (auto* light = _sun->findComponent<LightComponent>()) {
            light->setEnabled(_sunOn);
        }
        for (const auto& lamp : _lamps) {
            lamp.light->setEnabled(_lampsOn);
            lamp.bulb->setEnabled(_lampsOn);
        }
        for (const auto& spot : _spots) {
            spot.light->setEnabled(_spotsOn);
        }
        auto rendering = _cameraComp->rendering();
        rendering.composeDebug = kViews[static_cast<size_t>(_view)];
        _cameraComp->setRendering(rendering);
    }

    std::unique_ptr<Asset> _rocks;
    std::vector<std::unique_ptr<Asset>> _shapeAssets;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<StandardMaterial> _wallMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<LensFlareEffect> _lensFlare;
    std::shared_ptr<LensFlare> _sunFlare;
    std::vector<std::shared_ptr<LensFlare>> _flares;
    std::vector<Lamp> _lamps;
    std::vector<Spot> _spots;
    Entity* _sun = nullptr;
    CameraComponent* _cameraComp = nullptr;

    float _intensity = 0.25f;
    float _scale = 0.5f;
    float _occlusionRadius = 0.02f;
    float _occlusionFade = 0.15f;
    float _edgeFade = 0.2f;
    bool _allowOffScreen = true;
    bool _sunOn = true;
    bool _lampsOn = true;
    bool _spotsOn = true;
    int _view = 0;
    float _sweep = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(LensFlareExample)
