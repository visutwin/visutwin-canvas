// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.07.2026
//
// Port of upstream camera-frame/post-processing.
//
// Every camera frame effect in the area lights room (area-lights-room.glb): a closed room lit by
// three glowing doors with shadowed spot lights, a drifting orb light and the sun through the
// skylight (its shadow fitted to the room), a little ambient light and the sky as the camera's clear
// colour (scaled by the sky brightness, so above 1 it blooms). The statue, a glossy red sphere and a
// white ceramic one stand on the pedestals. Two lines of text sit on a screen: one on the World
// layer with a colour brighter than 1, post-processed and blooming, one on the UI layer, rendered
// after the post-processing. The effects: anti-aliasing (MSAA, SMAA, both, TAA, TAA upscaling,
// sharpening), bloom (threshold, high quality), SSAO, depth of field, volumetric fog scattering the
// sun and the local lights, lens flares on the orb and the doors, auto exposure, colour grading
// with its channel mixer (N steps through upstream's presets),
// colour enhance, a 3D colour LUT, the vignette and fringing. Only bloom, the vignette and MSAA are
// on to start with.
//
// Keys stand in for upstream's controls panel:
//   E            the camera frame on or off            M   tone mapping
//   A            anti-aliasing: none, MSAA, SMAA, MSAA + SMAA, TAA, TAA upscale
//   1 / 2        render scale -/+ 0.1 (0.5 - 1)        3 / 4   sharpening -/+ 0.1
//   B            bloom                                  [ / ]   bloom intensity -/+ 5
//   H            bloom high quality                     J / K   bloom threshold -/+ 0.5
//   O            SSAO                                   D       depth of field
//   F            volumetric fog                         L       lens flares
//   X            auto exposure                          G       colour grading
//   N            channel mixer preset (none, monochrome, sepia, swap red and blue, infrared film,
//                night vision, deep sea, red planet, neon nights, alien planet, opposite hues)
//   C            colour enhance                         U       LUT: off, then each of five
//   V            vignette                               I       fringing
//   Z            debug view: none, bloom, ssao, dofcoc, dofblur, scene, depth
//   - / =        sky brightness -/+ 2 (0 - 20)          9 / 0   emissive -/+ 0.25 (0 - 3)
//   T            the text                               Space   pause the orb
//
// DEVIATIONS:
// - The camera frame cannot be removed here: E switches every effect off and the scale to 1, so
//   the camera renders through the plain forward path, as upstream's disabled CameraFrame does.
// - The clustered shadow atlas has no scene-wide filter type (upstream sets PCF5 16-bit).
// - The glTF materials are found from the instantiated meshes rather than the container's list.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/autoExposureEffect.h"
#include "scene/graphics/lensFlareEffect.h"
#include "scene/graphics/smaaEffect.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr std::array<const char*, 7> kFlareShapes = {"glow_disc", "spike_6", "iris_round", "iris_hexagon",
        "anamorphic_glow", "anamorphic_streak", "glint"};
    constexpr std::array<const char*, 5> kLuts = {"lut-teal-orange", "lut-late-sunset", "lut-moonlight", "lut-sofia",
        "lut-bw"};
    constexpr std::array<const char*, 7> kDebugViews = {"", "bloom", "ssao", "dofcoc", "dofblur", "scene", "depth"};

    // The channel mixer presets of the grading: the weights of the red, green and blue input
    // channels in the red, green and blue output channels (upstream's presets, selected in turn).
    struct MixerPreset
    {
        const char* name;
        float red[3];
        float green[3];
        float blue[3];
    };
    constexpr std::array<MixerPreset, 11> kMixerPresets = {{
        {"none", {1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
        // black and white, using the luminance weights of linear sRGB in all three output channels
        {"monochrome", {0.2126f, 0.7152f, 0.0722f}, {0.2126f, 0.7152f, 0.0722f}, {0.2126f, 0.7152f, 0.0722f}},
        {"sepia", {0.393f, 0.769f, 0.189f}, {0.349f, 0.686f, 0.168f}, {0.272f, 0.534f, 0.131f}},
        {"swapRedBlue", {0, 0, 1}, {0, 1, 0}, {1, 0, 0}},
        // false-colour infrared film: greens turn red, and reds turn green
        {"infraredFilm", {0.2f, 1.0f, 0.0f}, {0.9f, 0.1f, 0.0f}, {0.0f, 0.1f, 0.9f}},
        // the luminance, boosted into the green channel
        {"nightVision", {0.06f, 0.21f, 0.02f}, {0.43f, 1.43f, 0.14f}, {0.06f, 0.21f, 0.02f}},
        // the red light is absorbed, as deep under water
        {"deepSea", {0.15f, 0.1f, 0.0f}, {0.15f, 0.85f, 0.15f}, {0.2f, 0.35f, 0.95f}},
        // a dusty orange world under an orange sky
        {"redPlanet", {0.6f, 0.6f, 0.2f}, {0.25f, 0.35f, 0.1f}, {0.05f, 0.1f, 0.1f}},
        // neon lights: warm colours turn hot pink, and cool colours violet
        {"neonNights", {1.0f, 0.0f, 0.6f}, {0.0f, 0.6f, 0.2f}, {0.3f, 0.4f, 1.2f}},
        // the channels rotated: reds turn blue, greens turn red and blues turn green
        {"alienPlanet", {0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        // every hue rotated half way around the colour wheel, while the greys stay grey
        {"oppositeHues", {-0.333f, 0.667f, 0.667f}, {0.667f, -0.333f, 0.667f}, {0.667f, 0.667f, -0.333f}}
    }};
    constexpr std::array<int, 8> kToneMappings = {TONEMAP_LINEAR, TONEMAP_FILMIC, TONEMAP_HEJL, TONEMAP_ACES,
        TONEMAP_ACES2, TONEMAP_NEUTRAL, TONEMAP_AGX, TONEMAP_AGX_PUNCHY};

    struct AaMethod
    {
        const char* name;
        int samples;
        bool smaa;
        bool taa;
        bool upscale;
    };
    constexpr std::array<AaMethod, 6> kAaMethods = {{
        {"none", 1, false, false, false},
        {"msaa", 4, false, false, false},
        {"smaa", 1, true, false, false},
        {"msaa + smaa", 4, true, false, false},
        {"taa", 1, false, true, false},
        {"taa upscale", 1, false, true, true},
    }};

    LensFlareElement element(const char* shape, const float size, const float intensity)
    {
        LensFlareElement e;
        e.shape = shape;
        e.size = size;
        e.intensity = intensity;
        return e;
    }

    // The rotation that turns `from` onto `to` (both unit length).
    Quaternion rotationBetween(const Vector3& from, const Vector3& to)
    {
        const float d = std::clamp(from.dot(to), -1.0f, 1.0f);
        const Vector3 axis = from.cross(to);
        if (axis.length() < 1e-6f) {
            return Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
        }
        return Quaternion::fromAxisAngle(axis.normalized(), std::acos(d) * 180.0f / std::numbers::pi_v<float>);
    }

    // The settings, at upstream's initial values.
    struct Settings
    {
        bool frame = true;
        float scale = 1.0f;
        int toneMapping = TONEMAP_ACES;
        int debug = 0;
        int aaMethod = 1;   // msaa
        float sharpness = 0.0f;
        bool bloom = true;
        float bloomIntensity = 30.0f;
        int bloomBlurLevel = 6;
        float bloomThreshold = 0.0f;
        bool bloomHighQuality = false;
        bool ssao = false;
        bool dof = false;
        bool fog = false;
        bool lensFlare = false;
        bool autoExposure = false;
        bool grading = false;
        int mixer = 0;   // kMixerPresets
        bool colorEnhance = false;
        int lut = -1;   // off
        bool vignette = true;
        bool fringing = false;
        float sky = 10.0f;
        float emissive = 1.0f;
        bool animate = true;
        bool text = true;
    };
}

class PostProcessingExample final : public ExampleApp
{
public:
    // The scene renders to an antialiased texture, so the back buffer, showing only the UI drawn
    // after the post-processing, needs none.
    PostProcessingExample() : ExampleApp({.title = "Post-Processing", .width = 1280, .height = 720, .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _room = std::make_unique<Asset>("room", AssetType::CONTAINER, assetPath("models/area-lights-room.glb"));
        _statue = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        for (const char* name : kFlareShapes) {
            _flareAssets.push_back(std::make_unique<Asset>(name, AssetType::TEXTURE,
                assetPath(std::string("textures/lens-flare/") + name + ".png")));
        }
        for (const char* name : kLuts) {
            _lutAssets.push_back(std::make_unique<Asset>(name, AssetType::TEXTURE,
                assetPath(std::string("cube-luts/") + name + ".png"), AssetData{.mipmaps = false}));
        }
        auto* roomContainer = _room->resourceAs<ContainerResource>();
        auto* statueContainer = _statue->resourceAs<ContainerResource>();
        if (!roomContainer || !statueContainer) {
            spdlog::error("post-processing: a model did not load");
            return false;
        }

        auto& lighting = scene()->lighting();
        lighting.shadowsEnabled = true;
        lighting.shadowAtlasResolution = 2048;

        // The room is closed: no environment lighting, only a little ambient light.
        scene()->setAmbientLight(0.03f, 0.032f, 0.04f);

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
            }
        }
        createSphere(Vector3(0.0f, 1.26f, 0.0f), 0.9f, Color(0.7f, 0.02f, 0.02f, 1.0f), 0.85f);
        createSphere(Vector3(4.0f, 1.15f, 0.8f), 0.6f, Color(0.9f, 0.9f, 0.88f, 1.0f), 0.7f);

        // The glowing doors, each with a shadowed spot light shining into the room (down its -Y
        // axis, turned to face it).
        const struct
        {
            float x;
            Color color;
        } doors[] = {
            {-4.5f, Color(1.0f, 0.42f, 0.12f, 1.0f)},
            {0.0f, Color(0.25f, 0.8f, 1.0f, 1.0f)},
            {4.5f, Color(1.0f, 0.8f, 0.35f, 1.0f)},
        };
        for (const auto& door : doors) {
            Entity* panel = createPrimitive("box", createEmissiveMaterial(door.color, 4.0f), Vector3(door.x, 1.7f, -9.45f),
                Vector3(1.7f, 3.3f, 0.02f));
            if (auto* render = panel->findComponent<RenderComponent>()) {
                render->setCastShadows(false);
            }
            auto* light = new Entity();
            light->setEngine(engine());
            if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
                lc->setType(LightType::LIGHTTYPE_SPOT);
                lc->setColor(door.color);
                lc->setIntensity(1.5f);
                lc->setRange(20.0f);
                lc->setInnerConeAngle(50.0f);
                lc->setOuterConeAngle(80.0f);
                lc->setCastShadows(true);
                lc->setShadowBias(0.2f);
                lc->setShadowNormalBias(0.05f);
                _doorLights.push_back(lc);
            }
            light->setLocalPosition(door.x, 1.7f, -9.2f);
            light->setLocalEulerAngles(-90.0f, 0.0f, 0.0f);
            root()->addChild(light);
        }

        // A glowing orb drifting around the room.
        _orb = createPrimitive("sphere", createEmissiveMaterial(Color(0.8f, 0.88f, 1.0f, 1.0f), 6.0f),
            Vector3(0.0f, 2.2f, 3.7f), Vector3(0.4f, 0.4f, 0.4f));
        _orb->setName("Orb");
        if (auto* render = _orb->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }
        if (auto* lc = static_cast<LightComponent*>(_orb->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_OMNI);
            lc->setColor(Color(0.8f, 0.88f, 1.0f, 1.0f));
            lc->setIntensity(1.5f);
            lc->setRange(10.0f);
            lc->setCastShadows(true);
            lc->setShadowBias(0.2f);
            lc->setShadowNormalBias(0.05f);
            _orbLight = lc;
        }

        // The sun through the skylight, which the fog scatters into light shafts; its shadow
        // covers just the room. It shines down its -Y axis, turned to the sun's direction.
        auto* sun = new Entity();
        sun->setName("Sun");
        sun->setEngine(engine());
        if (auto* lc = static_cast<LightComponent*>(sun->addComponent<LightComponent>())) {
            lc->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            lc->setColor(Color(1.0f, 0.86f, 0.68f, 1.0f));
            lc->setIntensity(2.5f);
            lc->setCastShadows(true);
            lc->setShadowType(SHADOW_PCF3_32F);
            lc->setShadowResolution(2048);
            lc->setShadowBias(0.2f);
            lc->setShadowNormalBias(0.02f);
            lc->setShadowBounds(BoundingBox(Vector3(0.0f, 3.4f, 0.0f), Vector3(8.6f, 3.4f, 9.6f)));
        }
        sun->setRotation(rotationBetween(Vector3(0.0f, -1.0f, 0.0f), Vector3(0.13f, -0.93f, 0.35f).normalized()));
        root()->addChild(sun);

        // The camera at the front of the room, looking at the doors.
        Entity* camera = createCamera(Vector3(1.3f, 2.5f, 6.8f));
        _cameraComp = camera->findComponent<CameraComponent>();
        _cameraComp->camera()->setFov(60.0f);
        _cameraComp->camera()->setNearClip(0.1f);
        _cameraComp->camera()->setFarClip(100.0f);
        if (CameraControls* controls = addOrbitControls(camera, Vector3(0.0f, 1.5f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-38.0f, 8.0f));
            controls->setZoomRange(Vector2(2.0f, 7.0f));
            controls->storeResetState();
        }

        createText();

        // The volumetric fog fills the room, scattering the sun's light and the local lights'.
        auto fog = _cameraComp->volumetricFog();
        fog.maxDistance = 30.0f;
        fog.heightFalloff = 0.1f;
        fog.ambientColor[0] = 0.4f;
        fog.ambientColor[1] = 0.5f;
        fog.ambientColor[2] = 0.7f;
        fog.ambientIntensity = 0.01f;
        _cameraComp->setVolumetricFog(fog);

        // SMAA, the alternative to the multisampling and TAA.
        _smaa = std::make_shared<SmaaEffect>();
        _cameraComp->addFrameEffect(_smaa);

        // The lens flares of the orb and the doors: each look places its elements along the axis
        // from the light through the screen centre (0 on the light, 1 at the centre, 2 mirrored
        // across it), sized as fractions of the screen height.
        auto orbLook = std::make_shared<LensFlareLook>();
        orbLook->elements.push_back(element("glow_disc", 0.14f, 0.4f));
        orbLook->elements.push_back(element("spike_6", 0.4f, 0.5f));
        orbLook->elements.push_back(element("iris_round", 0.035f, 0.15f));
        orbLook->elements.back().offset = 0.65f;
        orbLook->elements.back().count = 2;
        orbLook->elements.back().spread = 0.25f;
        orbLook->elements.push_back(element("iris_hexagon", 0.06f, 0.15f));
        orbLook->elements.back().offset = 1.5f;
        auto doorLook = std::make_shared<LensFlareLook>();
        doorLook->elements.push_back(element("anamorphic_glow", 0.35f, 0.2f));
        doorLook->elements.back().aspect = 2.5f;
        doorLook->elements.push_back(element("anamorphic_streak", 0.06f, 0.17f));
        doorLook->elements.back().aspect = 22.0f;
        doorLook->elements.push_back(element("glint", 0.5f, 0.07f));

        _lensFlare = std::make_shared<LensFlareEffect>();
        LensFlareShapeLibrary library;
        library.resolution = 512;
        for (const auto& asset : _flareAssets) {
            library.shapes.emplace_back(asset->name(), asset->resourceAs<Texture>());
        }
        _lensFlare->setShapes(library);
        auto orbFlare = std::make_shared<LensFlare>(orbLook, LensFlareSource{.light = _orbLight});
        orbFlare->occlusionOffset = 0.3f;
        _lensFlare->add(orbFlare);
        _flares.push_back(orbFlare);
        for (auto* light : _doorLights) {
            // The light sits behind its glowing door, so its visibility is tested in front of it.
            auto flare = std::make_shared<LensFlare>(doorLook, LensFlareSource{.light = light});
            flare->occlusionOffset = 0.5f;
            _lensFlare->add(flare);
            _flares.push_back(flare);
        }
        _cameraComp->addFrameEffect(_lensFlare);

        // Auto exposure, adapting the exposure to the brightness of the scene.
        _autoExposure = std::make_shared<AutoExposureEffect>();
        _autoExposure->compensation = -2.5f;
        _autoExposure->speedDarkToLight = 3.0f;
        _autoExposure->speedLightToDark = 1.0f;
        _cameraComp->addFrameEffect(_autoExposure);

        applySettings();
        spdlog::info("post-processing: E frame, M tone mapping, A anti-aliasing, 1/2 scale, 3/4 sharpening, B bloom, "
                     "[ ] intensity, H high quality, J/K threshold, O SSAO, D DOF, F fog, L lens flares, X auto "
                     "exposure, G grading, N channel mixer, C colour enhance, U LUT, V vignette, I fringing, Z debug, -/= sky, 9/0 "
                     "emissive, T text, Space pause");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        auto& s = _settings;
        switch (event.key.key) {
        case SDLK_E: s.frame = !s.frame; break;
        case SDLK_M: {
            const auto it = std::find(kToneMappings.begin(), kToneMappings.end(), s.toneMapping);
            const size_t index = it == kToneMappings.end() ? 0 : static_cast<size_t>(it - kToneMappings.begin()) + 1;
            s.toneMapping = kToneMappings[index % kToneMappings.size()];
            break;
        }
        case SDLK_A: s.aaMethod = (s.aaMethod + 1) % static_cast<int>(kAaMethods.size()); break;
        case SDLK_1: s.scale = std::max(0.5f, s.scale - 0.1f); break;
        case SDLK_2: s.scale = std::min(1.0f, s.scale + 0.1f); break;
        case SDLK_3: s.sharpness = std::max(0.0f, s.sharpness - 0.1f); break;
        case SDLK_4: s.sharpness = std::min(1.0f, s.sharpness + 0.1f); break;
        case SDLK_B: s.bloom = !s.bloom; break;
        case SDLK_LEFTBRACKET: s.bloomIntensity = std::max(0.0f, s.bloomIntensity - 5.0f); break;
        case SDLK_RIGHTBRACKET: s.bloomIntensity = std::min(100.0f, s.bloomIntensity + 5.0f); break;
        case SDLK_H: s.bloomHighQuality = !s.bloomHighQuality; break;
        case SDLK_J: s.bloomThreshold = std::max(0.0f, s.bloomThreshold - 0.5f); break;
        case SDLK_K: s.bloomThreshold = std::min(10.0f, s.bloomThreshold + 0.5f); break;
        case SDLK_O: s.ssao = !s.ssao; break;
        case SDLK_D: s.dof = !s.dof; break;
        case SDLK_F: s.fog = !s.fog; break;
        case SDLK_L: s.lensFlare = !s.lensFlare; break;
        case SDLK_X: s.autoExposure = !s.autoExposure; break;
        case SDLK_G: s.grading = !s.grading; break;
        case SDLK_N: s.mixer = (s.mixer + 1) % static_cast<int>(kMixerPresets.size()); break;
        case SDLK_C: s.colorEnhance = !s.colorEnhance; break;
        case SDLK_U: s.lut = s.lut + 1 >= static_cast<int>(kLuts.size()) ? -1 : s.lut + 1; break;
        case SDLK_V: s.vignette = !s.vignette; break;
        case SDLK_I: s.fringing = !s.fringing; break;
        case SDLK_Z: s.debug = (s.debug + 1) % static_cast<int>(kDebugViews.size()); break;
        case SDLK_MINUS: s.sky = std::max(0.0f, s.sky - 2.0f); break;
        case SDLK_EQUALS: s.sky = std::min(20.0f, s.sky + 2.0f); break;
        case SDLK_9: s.emissive = std::max(0.0f, s.emissive - 0.25f); break;
        case SDLK_0: s.emissive = std::min(3.0f, s.emissive + 0.25f); break;
        case SDLK_T: s.text = !s.text; break;
        case SDLK_SPACE: s.animate = !s.animate; break;
        default: return false;
        }
        applySettings();
        return true;
    }

    void update(const float dt) override
    {
        if (_settings.animate) {
            _time += dt;
        }
        // The orb circles the pedestals, bobbing up and down.
        const float orbAngle = _time * 0.4f;
        _orb->setPosition(std::sin(orbAngle) * 5.5f, 2.2f + std::sin(_time * 1.1f) * 0.6f, 0.5f + std::cos(orbAngle) * 3.2f);
    }

private:
    struct Emissive
    {
        StandardMaterial* material;
        float intensity;
    };

    void createSphere(const Vector3& position, const float radius, const Color& color, const float gloss)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setUseMetalness(true);
        material->setMetalness(0.0f);
        material->setGloss(gloss);
        _materials.push_back(material);
        createPrimitive("sphere", material.get(), position, Vector3(radius * 2.0f, radius * 2.0f, radius * 2.0f))
            ->setName("Sphere");
    }

    // An emissive material whose intensity the emissive setting scales.
    StandardMaterial* createEmissiveMaterial(const Color& color, const float intensity)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setUseLighting(false);
        material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        material->setEmissive(color);
        _materials.push_back(material);
        _emissives.push_back({material.get(), intensity});
        return material.get();
    }

    void createText()
    {
        auto* font = _font->resourceAs<FontResource>();
        if (!font) {
            spdlog::warn("post-processing: the font did not load; no text");
            return;
        }
        _screen = createScreen();
        const auto addText = [&](const std::string& text, const float y, const int layer, const Color& color) {
            auto* entity = new Entity();
            entity->setName(text);
            entity->setEngine(engine());
            if (auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>())) {
                element->setup({.type = ElementType::Text, .anchor = Vector4(0.5f, y, 0.5f, y), .pivot = Vector2(0.5f, 0.5f)});
                element->setFontResource(font);
                element->setFontSize(22);
                element->setText(text);
                element->setColor(color);
                element->setLayers({layer});
            }
            _screen->entity()->addChild(entity);
        };
        // The World layer's text is brighter than 1, so it blooms: the UI has no emissive intensity.
        addText("Text on the World layer, post-processed", 0.09f, LAYERID_WORLD, Color(5.0f, 2.2f, 0.4f, 1.0f));
        addText("Text on the UI layer, rendered after the post-processing", 0.04f, LAYERID_UI,
            Color(1.0f, 1.0f, 1.0f, 1.0f));
    }

    void applySettings()
    {
        const auto& s = _settings;
        const bool on = s.frame;
        const AaMethod& method = kAaMethods[static_cast<size_t>(s.aaMethod)];

        auto rendering = _cameraComp->rendering();
        rendering.renderTargetScale = on ? s.scale : 1.0f;
        rendering.composeDebug = on ? kDebugViews[static_cast<size_t>(s.debug)] : "";
        rendering.samples = on ? method.samples : 1;
        rendering.sharpness = on ? s.sharpness : 0.0f;
        rendering.bloomIntensity = on && s.bloom ? 0.1f * (s.bloomIntensity / 100.0f) : 0.0f;
        rendering.bloomBlurLevel = s.bloomBlurLevel;
        rendering.bloomThreshold = s.bloomThreshold;
        rendering.bloomHighQuality = s.bloomHighQuality;

        rendering.gradingEnabled = on && s.grading;
        rendering.gradingSaturation = 1.0f;
        rendering.gradingBrightness = 1.0f;
        rendering.gradingContrast = 1.0f;
        // the channel mixer preset: the weights of each output channel
        const MixerPreset& mixer = kMixerPresets[static_cast<size_t>(s.mixer)];
        std::copy(std::begin(mixer.red), std::end(mixer.red), rendering.gradingMixerRed);
        std::copy(std::begin(mixer.green), std::end(mixer.green), rendering.gradingMixerGreen);
        std::copy(std::begin(mixer.blue), std::end(mixer.blue), rendering.gradingMixerBlue);
        // Colour enhance and grading start at upstream's neutral values (no change) when switched on.
        rendering.colorEnhanceShadows = 0.0f;
        rendering.colorEnhanceHighlights = 0.0f;
        rendering.colorEnhanceMidtones = 0.0f;
        rendering.colorEnhanceVibrance = 0.0f;
        rendering.colorEnhanceDehaze = 0.0f;
        rendering.colorLUT = on && s.lut >= 0 ? _lutAssets[static_cast<size_t>(s.lut)]->resourceAs<Texture>() : nullptr;
        rendering.colorLUTIntensity = 1.0f;
        rendering.vignetteEnabled = on && s.vignette;
        rendering.vignetteInner = 0.3f;
        rendering.vignetteOuter = 1.0f;
        rendering.vignetteCurvature = 0.5f;
        rendering.vignetteIntensity = 0.6f;
        rendering.fringingIntensity = on && s.fringing ? 50.0f : 0.0f;
        _cameraComp->setRendering(rendering);
        _cameraComp->setToneMapping(s.toneMapping);

        auto taa = _cameraComp->taa();
        taa.enabled = on && method.taa;
        taa.upscale = method.upscale;
        _cameraComp->setTaa(taa);
        _smaa->setEnabled(on && method.smaa);

        auto ssao = _cameraComp->ssao();
        ssao.enabled = on && s.ssao;
        ssao.type = "lighting";
        ssao.intensity = 0.5f;
        ssao.radius = 0.8f;
        ssao.samples = 12;
        ssao.blurEnabled = true;
        _cameraComp->setSsao(ssao);

        auto dof = _cameraComp->dof();
        dof.enabled = on && s.dof;
        dof.focusDistance = 7.0f;
        dof.focusRange = 1.5f;
        dof.blurRadius = 4.0f;
        dof.nearBlur = false;
        dof.highQuality = true;
        _cameraComp->setDof(dof);

        auto fog = _cameraComp->volumetricFog();
        fog.enabled = on && s.fog;
        fog.density = 0.1f;
        fog.intensity = 3.0f;
        fog.anisotropy = 0.0f;
        fog.localSpotLights = true;
        fog.localOmniLights = true;
        _cameraComp->setVolumetricFog(fog);

        _lensFlare->setEnabled(on && s.lensFlare);
        _autoExposure->setEnabled(on && s.autoExposure);

        for (const auto& emissive : _emissives) {
            emissive.material->setEmissiveIntensity(emissive.intensity * s.emissive);
        }
        if (_screen) {
            _screen->entity()->setEnabled(s.text);
        }
        // The sky through the skylight and the windows; brighter than 1, it blooms.
        const Color sky(0.45f, 0.6f, 0.8f, 1.0f);
        _cameraComp->camera()->setClearColor(Color(sky.r * s.sky, sky.g * s.sky, sky.b * s.sky, 1.0f));

        spdlog::info("[settings] frame={} aa={} scale={:.1f} sharpen={:.1f} bloom={}({:.0f}, threshold {:.1f}{}) ssao={} "
                     "dof={} fog={} flares={} autoExposure={} grading={}(mixer {}) enhance={} lut={} vignette={} "
                     "fringing={} debug={} sky={:.0f} emissive={:.2f}",
            s.frame, method.name, s.scale, s.sharpness, s.bloom, s.bloomIntensity, s.bloomThreshold,
            s.bloomHighQuality ? ", hq" : "", s.ssao, s.dof, s.fog, s.lensFlare, s.autoExposure, s.grading,
            kMixerPresets[static_cast<size_t>(s.mixer)].name, s.colorEnhance, s.lut >= 0 ? kLuts[static_cast<size_t>(s.lut)] : "off", s.vignette, s.fringing,
            kDebugViews[static_cast<size_t>(s.debug)], s.sky, s.emissive);
    }

    Settings _settings;
    std::unique_ptr<Asset> _room;
    std::unique_ptr<Asset> _statue;
    std::unique_ptr<Asset> _font;
    std::vector<std::unique_ptr<Asset>> _flareAssets;
    std::vector<std::unique_ptr<Asset>> _lutAssets;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Emissive> _emissives;
    std::vector<LightComponent*> _doorLights;
    LightComponent* _orbLight = nullptr;
    Entity* _orb = nullptr;
    CameraComponent* _cameraComp = nullptr;
    ScreenComponent* _screen = nullptr;
    std::shared_ptr<SmaaEffect> _smaa;
    std::shared_ptr<LensFlareEffect> _lensFlare;
    std::vector<std::shared_ptr<LensFlare>> _flares;
    std::shared_ptr<AutoExposureEffect> _autoExposure;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(PostProcessingExample)
