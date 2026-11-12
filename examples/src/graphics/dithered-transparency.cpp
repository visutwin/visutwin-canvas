// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.11.2026
//
// Port of upstream graphics/dithered-transparency.
//
// A procedural glass display on a dark polished floor (a 300 x 300 plane, grey (0.4, 0.4, 0.44),
// gloss 0.1) against a near-black void: a metal plinth with a glowing cyan rim, a gold cone spire
// turning at 25 degrees a second with a double helix of 2 x 28 glowing orbs around it (hues
// along the strands, emissive intensity 1), a blue-tinted glass cylinder dome enclosing the
// sculpture, and two tinted glass panels in front of it (pink, turned 8 degrees, and amber,
// turned -8). The three glass materials are alpha-blended (normal blending) with
// opacityFadesSpecular off, so their reflections stay at full strength, AND opacity-dithered,
// surface and shadow, with the dither strength decoupled from the opacity (alphaDither). Every
// mesh offsets the dither pattern by its own position, so where the glass layers overlap, and
// where their shadows overlap on the floor, the coverage accumulates instead of repeating the
// same holes.
//
// The studio env atlas lights the reflections only (exposure 1.2, skybox intensity 0.1, the
// Skybox layer off). A warm directional light from behind and above (66, 200, 0), intensity 1.3,
// casts soft VSM shadows (2048, blur 20, bias 0.1, normal bias 0.1) of the glass and the sculpture
// onto the floor in front of the display, and two coloured omni fill lights (pink at x = -10,
// blue at x = 10, intensity 0.8, range 30) pool light on the floor either side. The camera (fov
// 55) starts at (-0.0168, 12.6637, 30.0562) and orbits (4.8, 2, 9.6), pitch within +/-90 degrees
// and zoom up to 40. The camera frame runs ACES tone mapping, a scene colour map, bloom 0.015,
// vignette 0.5 and TAA jitter 1 (TAA itself off; when on, sharpness 1).
//
// Initial values, upstream's: opacity 1 (the transparency comes from the dither alone),
// alphaDither 0.3, Bayer16 for both the surface and the shadow, TAA off.
//
// Keys stand in for upstream's controls panel, over the same parameters, applied to all three
// glass materials:
//   - / =  opacity -/+ 0.05, 0..1
//   [ / ]  alpha dither -/+ 0.05, 0..1
//   C      dither colour (surface) pattern, cycled None / Bayer2 / Bayer4 / Bayer8 / Bayer16 /
//          BlueNoise / IGNNoise
//   S      dither shadow pattern, cycled the same way
//   T      TAA on / off (sharpness 1 while on, 0 off)
//   R reset camera | F1 HUD | Esc quit | LMB/RMB orbit, Shift/MMB pan, Wheel zoom
//
// DEVIATION: the sun casts SHADOW_VSM_16F; upstream asks for SHADOW_VSM_32F, which the engine
// does not have (VSM here is 16-bit EVSM only).
// DEVIATION: upstream renders at the display's native pixel ratio; the examples harness renders
// one pixel per point, as every example here does (VISUTWIN_MAX_PIXEL_RATIO=2 for full Retina).
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "platform/graphics/blendState.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float INITIAL_OPACITY = 1.0f;
    constexpr float INITIAL_ALPHA_DITHER = 0.3f;
    constexpr DitherMode INITIAL_DITHER = DitherMode::DITHER_BAYER16;
    constexpr DitherMode INITIAL_SHADOW_DITHER = DitherMode::DITHER_BAYER16;
    constexpr bool INITIAL_TAA = false;

    constexpr std::array<std::string_view, 7> kDitherNames = {
        "None", "Bayer2", "Bayer4", "Bayer8", "Bayer16", "BlueNoise", "IGNNoise"
    };

    DitherMode nextDither(const DitherMode mode)
    {
        return static_cast<DitherMode>((static_cast<int>(mode) + 1) % static_cast<int>(kDitherNames.size()));
    }

    std::string_view ditherName(const DitherMode mode)
    {
        return kDitherNames[static_cast<size_t>(mode)];
    }

    /// A saturated colour from a hue in 0..1.
    Color hueToColor(const float hue)
    {
        const auto f = [hue](const float n) {
            const float k = std::fmod(n + hue * 6.0f, 6.0f);
            return 1.0f - std::max(0.0f, std::min({k, 4.0f - k, 1.0f}));
        };
        return Color(f(5.0f), f(3.0f), f(1.0f));
    }
}

class DitheredTransparencyExample final: public ExampleApp
{
public:
    // TAA smooths the edges, so the back buffer is not multisampled.
    DitheredTransparencyExample(): ExampleApp({.antialias = false}) {}

protected:
    bool create() override
    {
        // Environment, used for reflections only: the backdrop is a dark void
        _envAtlasAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (Texture* envAtlas = _envAtlasAsset->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        } else {
            spdlog::error("Failed to load cubemaps/studio-env-atlas.png");
            return false;
        }
        scene()->setExposure(1.2f);
        scene()->setSkyboxIntensity(0.1f);
        if (const auto layers = scene()->layers()) {
            if (const auto skyLayer = layers->getLayerById(LAYERID_SKYBOX)) {
                skyLayer->setEnabled(false);
            }
        }

        _glassBlend = std::make_shared<BlendState>(BlendState::alphaBlend());

        // Dark polished floor
        createChildPrimitive(root(), "plane", createMaterial(Color(0.4f, 0.4f, 0.44f), 0.0f, 0.1f),
            Vector3(0.0f, 0.0f, 0.0f), Vector3(300.0f, 1.0f, 300.0f));

        _metal = createMaterial(Color(0.9f, 0.9f, 0.95f), 1.0f, 0.9f);
        _gold = createMaterial(Color(1.0f, 0.72f, 0.3f), 1.0f, 0.85f);

        buildDisplay(Vector3(0.0f, 0.0f, 0.0f));

        // Camera
        Entity* cameraEntity = createCamera(Vector3(-0.0168f, 12.6637f, 30.0562f),
            Vector3(-26.9155f, -13.2501f, 0.0f));
        _cameraComp = cameraEntity->findComponent<CameraComponent>();
        if (!_cameraComp || !_cameraComp->camera()) {
            spdlog::error("The camera has no camera component");
            return false;
        }
        _cameraComp->camera()->setFov(55.0f);
        _cameraComp->camera()->setClearColor(Color(0.005f, 0.006f, 0.012f, 1.0f));

        if (CameraControls* controls = addOrbitControls(cameraEntity, Vector3(4.8f, 2.0f, 9.6f))) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.01f, 40.0f));
            controls->storeResetState();
        }

        // Camera frame: ACES, a scene colour map, TAA jitter 1, bloom and a vignette
        _cameraComp->setToneMapping(TONEMAP_ACES);
        _cameraComp->requestSceneColorMap(true);
        auto rendering = _cameraComp->rendering();
        rendering.bloomIntensity = 0.015f;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.5f;
        _cameraComp->setRendering(rendering);
        auto taa = _cameraComp->taa();
        taa.jitter = 1.0f;
        _cameraComp->setTaa(taa);

        applyMaterials();
        applySettings();

        spdlog::info("Controls: -/= opacity, [ ] alpha dither, C dither colour, S dither shadow, T TAA");
        logState("init");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }

        switch (event.key.key) {
        case SDLK_MINUS:
            _opacity = step(_opacity, -0.05f);
            break;
        case SDLK_EQUALS:
            _opacity = step(_opacity, 0.05f);
            break;
        case SDLK_LEFTBRACKET:
            _alphaDither = step(_alphaDither, -0.05f);
            break;
        case SDLK_RIGHTBRACKET:
            _alphaDither = step(_alphaDither, 0.05f);
            break;
        case SDLK_C:
            _dither = nextDither(_dither);
            break;
        case SDLK_S:
            _shadowDither = nextDither(_shadowDither);
            break;
        case SDLK_T:
            _taaEnabled = !_taaEnabled;
            break;
        default:
            return false;
        }

        applyMaterials();
        applySettings();
        logState("changed");
        return true;
    }

    void update(const float dt) override
    {
        for (Entity* spinner : _spinners) {
            spinner->rotate(0.0f, 25.0f * dt, 0.0f);
        }
    }

private:
    static float step(const float value, const float delta)
    {
        return std::clamp(std::round((value + delta) * 100.0f) / 100.0f, 0.0f, 1.0f);
    }

    /// A metalness-workflow material; with a glow, the colour is emissive at that intensity over a
    /// fifth of it as the diffuse.
    StandardMaterial* createMaterial(const Color& color, const float metalness, const float gloss,
        const float glow = 0.0f)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setUseMetalness(true);
        material->setMetalness(metalness);
        material->setGloss(gloss);
        if (glow > 0.0f) {
            material->setDiffuse(Color(color.r * 0.2f, color.g * 0.2f, color.b * 0.2f, color.a));
            material->setEmissive(color);
            material->setEmissiveIntensity(glow);
        }
        _materials.push_back(material);
        return material.get();
    }

    /// A glass material: normal alpha blending, with the reflections kept at full strength so only
    /// the tint fades with the opacity. The dither is set by applyMaterials.
    StandardMaterial* createGlass(const Color& tint)
    {
        StandardMaterial* material = createMaterial(tint, 0.0f, 0.95f);
        material->setBlendState(_glassBlend);
        material->setTransparent(true);
        material->setOpacityFadesSpecular(false);
        _glassMaterials.push_back(material);
        return material;
    }

    /// A primitive entity under `parent`, at a local position and scale.
    Entity* createChildPrimitive(Entity* parent, const char* type, Material* material,
        const Vector3& position, const Vector3& scale)
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setName(type);
        entity->setLocalPosition(position);
        entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
        if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
            render->setMaterial(material);
            render->setType(type);
        }
        parent->addChild(entity);
        return entity;
    }

    Entity* createChildEntity(Entity* parent, const char* name)
    {
        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setName(name);
        parent->addChild(entity);
        return entity;
    }

    /// The display: a glowing helix of orbs around a gold spire on an illuminated plinth, enclosed
    /// by a glass dome, with two tinted glass panels in front, so the dithering of several
    /// overlapping layers shows against fine, bright detail.
    void buildDisplay(const Vector3& position)
    {
        Entity* display = createChildEntity(root(), "display");
        display->setLocalPosition(position);

        // Plinth with a glowing rim
        createChildPrimitive(display, "cylinder", _metal, Vector3(0.0f, 0.25f, 0.0f), Vector3(12.8f, 0.5f, 12.8f));
        createChildPrimitive(display, "cylinder", createMaterial(Color(0.2f, 0.9f, 1.0f), 0.0f, 0.5f, 2.0f),
            Vector3(0.0f, 0.06f, 0.0f), Vector3(13.8f, 0.12f, 13.8f));

        // Rotating sculpture: a gold spire with a double helix of glowing orbs
        Entity* sculpture = createChildEntity(display, "sculpture");
        sculpture->setLocalPosition(0.0f, 0.5f, 0.0f);
        _spinners.push_back(sculpture);

        createChildPrimitive(sculpture, "cone", _gold, Vector3(0.0f, 2.6f, 0.0f), Vector3(1.8f, 5.2f, 1.8f));

        constexpr int count = 28;
        constexpr float pi = 3.14159265358979323846f;
        for (int strand = 0; strand < 2; ++strand) {
            for (int i = 0; i < count; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(count - 1);
                const float angle = t * pi * 4.0f + static_cast<float>(strand) * pi;
                const float radius = (1.9f - t * 0.9f) * 2.0f;
                const float size = (0.75f - t * 0.35f) * 1.5f;
                const float hue = std::fmod(t * 0.8f + static_cast<float>(strand) * 0.5f, 1.0f);
                createChildPrimitive(sculpture, "sphere", createMaterial(hueToColor(hue), 0.0f, 0.9f, 1.0f),
                    Vector3(std::cos(angle) * radius, 0.4f + t * 4.6f, std::sin(angle) * radius),
                    Vector3(size, size, size));
            }
        }

        // Glass dome enclosing the sculpture
        createChildPrimitive(display, "cylinder", createGlass(Color(0.5f, 0.8f, 1.0f)),
            Vector3(0.0f, 3.8f, 0.0f), Vector3(10.8f, 6.6f, 10.8f));

        // Tinted glass panels in front
        createChildPrimitive(display, "box", createGlass(Color(1.0f, 0.3f, 0.65f)),
            Vector3(-2.2f, 3.8f, 7.5f), Vector3(6.0f, 7.2f, 0.15f))->setLocalEulerAngles(0.0f, 8.0f, 0.0f);
        createChildPrimitive(display, "box", createGlass(Color(1.0f, 0.7f, 0.15f)),
            Vector3(2.2f, 3.8f, 9.0f), Vector3(6.0f, 7.2f, 0.15f))->setLocalEulerAngles(0.0f, -8.0f, 0.0f);

        // Directional light from behind and above, casting soft VSM shadows of the glass and the
        // sculpture onto the floor in front of the display, where the camera sees them. The blur
        // hides the dither pattern in the shadow map.
        Entity* sun = createChildEntity(display, "sun");
        if (auto* light = static_cast<LightComponent*>(sun->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            light->setColor(Color(1.0f, 0.95f, 0.9f));
            light->setIntensity(1.3f);
            light->setRange(200.0f);
            light->setCastShadows(true);
            light->setShadowResolution(2048);
            light->setShadowType(ShadowType::SHADOW_VSM_16F);
            light->setVsmBlurSize(20);
            light->setShadowBias(0.1f);
            light->setShadowNormalBias(0.1f);
        }
        sun->setLocalEulerAngles(66.0f, 200.0f, 0.0f);

        // Coloured fill lights from either side, spilling pools of light on the floor
        const std::array<Color, 2> fillColors = {Color(1.0f, 0.25f, 0.7f), Color(0.2f, 0.6f, 1.0f)};
        for (size_t i = 0; i < fillColors.size(); ++i) {
            Entity* fill = createChildEntity(display, "fill");
            if (auto* light = static_cast<LightComponent*>(fill->addComponent<LightComponent>())) {
                light->setType(LightType::LIGHTTYPE_OMNI);
                light->setColor(fillColors[i]);
                light->setIntensity(0.8f);
                light->setRange(30.0f);
            }
            fill->setLocalPosition(i ? 10.0f : -10.0f, 2.5f, 8.0f);
        }
    }

    void applyMaterials()
    {
        for (StandardMaterial* material : _glassMaterials) {
            material->setOpacity(_opacity);
            material->setAlphaDither(_alphaDither);
            material->setOpacityDitherMode(_dither);
            material->setOpacityShadowDitherMode(_shadowDither);
        }
    }

    void applySettings()
    {
        auto taa = _cameraComp->taa();
        taa.enabled = _taaEnabled;
        _cameraComp->setTaa(taa);
        auto rendering = _cameraComp->rendering();
        rendering.sharpness = _taaEnabled ? 1.0f : 0.0f;
        _cameraComp->setRendering(rendering);
    }

    void logState(const char* reason) const
    {
        spdlog::info("{}: opacity={:.2f}, alphaDither={:.2f}, ditherColor={}, ditherShadow={}, taa={}", reason,
            _opacity, _alphaDither, ditherName(_dither), ditherName(_shadowDither), _taaEnabled ? "on" : "off");
    }

    std::unique_ptr<Asset> _envAtlasAsset;

    std::shared_ptr<BlendState> _glassBlend;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;   // keeps every material alive
    std::vector<StandardMaterial*> _glassMaterials;
    StandardMaterial* _metal = nullptr;
    StandardMaterial* _gold = nullptr;

    std::vector<Entity*> _spinners;
    CameraComponent* _cameraComp = nullptr;

    float _opacity = INITIAL_OPACITY;
    float _alphaDither = INITIAL_ALPHA_DITHER;
    DitherMode _dither = INITIAL_DITHER;
    DitherMode _shadowDither = INITIAL_SHADOW_DITHER;
    bool _taaEnabled = INITIAL_TAA;
};

VISUTWIN_EXAMPLE_MAIN(DitheredTransparencyExample)
