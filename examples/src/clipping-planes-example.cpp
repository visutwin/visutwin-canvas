// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// Port of upstream graphics/clipping-planes.
//
// A turbofan on a display stand, cut away with clipping planes. Two planes through the engine
// axis remove a wedge from every part (90 degrees by default), and in intersection mode a third
// plane across the axis limits how far back the cut runs; on start the cut runs from the inlet
// back to the exhaust over three seconds. The back faces of the parts, drawn flat red with the
// same planes, cap the cut where the parts are solid. The spools turn slowly through the cut,
// the fan and its turbine slower than the core.
//
// A studio: the product studio env atlas lights the scene, a dark floor (gloss 0.45) fades into
// the backdrop through linear fog from 9 to 24, and a directional key light (intensity 1.6, a
// 2048 shadow over 16 units) shines down into the cut from the front left. The camera frame
// renders with neutral tone mapping, the scene colour grab (the acrylic stand refracts what is
// behind it), 4x MSAA, bloom 0.02 over 7 levels, combine SSAO (radius 0.4, 12 samples, half
// scale, intensity 0.5) and a vignette (0.4 to 1.2, intensity 0.45).
//
// Model and environment: generated for upstream's example (the turbofan's closed solids,
// Draco geometry, instanced blade rows and procedural KTX2 textures; the RGBP studio env atlas).
//
// Keys stand in for upstream's controls panel:
//   C      the cut on / off
//   [ / ]  the opening -/+ 10 degrees, 0..180
//   , / .  the rotation -/+ 15 degrees, -180..180
//   - / =  the length -/+ 0.1, 0..1
//   K      the section caps on / off
//   Space  the spools spinning on / off
//   A      anti-aliasing, cycled MSAA, SMAA, TAA, none
//   B      bloom on / off
//   O      ambient occlusion on / off
//   V      vignette on / off
//
// DEVIATIONS:
// - upstream's CameraControls (focus (0.2, 1.35, 0), scene size 6, pitch -85..5, zoom 3..16) is
//   the examples' CameraControls in orbit mode with the same focus, pitch and zoom; there is
//   no fly mode to size, and upstream's portrait-screen horizontal FOV is not needed in a
//   landscape window (fov 40).
// - the heat-tinted exhaust parts ('heat', KHR_materials_iridescence) take the extension's
//   factors only: iridescence textures are not supported, so the tint reads teal where
//   upstream's thickness map makes it gold.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/shape/plane.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "scene/constants.h"
#include "scene/graphics/smaaEffect.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The extent of the engine along its axis, from its position.
    constexpr float kInlet = -2.2f;
    constexpr float kExhaust = 2.6f;
    constexpr float kRevealSeconds = 3.0f;

    enum class AntiAliasing { Msaa, Smaa, Taa, None };
    constexpr std::array<const char*, 4> kAntiAliasingNames{"MSAA", "SMAA", "TAA", "none"};

    float smootherstep(const float t)
    {
        const float x = std::clamp(t, 0.0f, 1.0f);
        return x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
    }

    float radians(const float degrees) { return degrees * (std::numbers::pi_v<float> / 180.0f); }
}

class ClippingPlanesExample final : public ExampleApp
{
public:
    ClippingPlanesExample() : ExampleApp({.title = "Clipping Planes"}) {}

protected:
    bool create() override
    {
        _turbofanAsset = std::make_unique<Asset>("turbofan", AssetType::CONTAINER, assetPath("models/turbofan.glb"));
        _envAtlasAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/product-studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* turbofanResource = _turbofanAsset->resourceAs<ContainerResource>();
        Texture* envAtlas = _envAtlasAsset->resourceAs<Texture>();
        if (!turbofanResource || !envAtlas) {
            spdlog::error("clipping-planes needs turbofan.glb and the product studio env atlas");
            return false;
        }

        // A studio: the environment lights the scene, and the floor fades into a dark backdrop.
        // Fog takes an sRGB colour, while the camera clears its linear HDR target with the
        // colour as given.
        const Color backdrop(0.2f, 0.21f, 0.24f, 1.0f);
        scene()->setEnvAtlas(envAtlas);
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_LINEAR);
        scene()->setFogColor(backdrop);
        scene()->setFogLinear(9.0f, 24.0f);

        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.06f, 0.065f, 0.075f, 1.0f));
        _floorMaterial->setGloss(0.45f);
        Entity* floor = createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(60.0f, 1.0f, 60.0f));
        floor->setName("floor");
        if (auto* render = floor->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        Entity* turbofan = turbofanResource->instantiateRenderEntity();
        turbofan->setEngine(engine());
        for (auto* render : turbofan->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
        }
        root()->addChild(turbofan);
        _engineNode = dynamic_cast<Entity*>(turbofan->findByName("Engine"));
        _lowSpool = turbofan->findByName("LowSpool");
        _highSpool = turbofan->findByName("HighSpool");
        if (!_engineNode) {
            spdlog::error("clipping-planes: the turbofan has no 'Engine' node");
            return false;
        }

        // The key light shines down into the cut from the front left.
        Entity* light = createDirectionalLight(Vector3(45.0f, -50.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.6f, true);
        light->setName("light");
        if (auto* component = light->findComponent<LightComponent>()) {
            component->setShadowDistance(16.0f);
            component->setShadowResolution(2048);
            component->setShadowBias(0.2f);
            component->setShadowNormalBias(0.04f);
        }

        Entity* camera = createCamera(Vector3(-4.0f, 3.9f, 4.9f));
        camera->setName("camera");
        _cameraComp = camera->findComponent<CameraComponent>();
        _cameraComp->camera()->setClearColor(Color(gammaToLinear(backdrop.r), gammaToLinear(backdrop.g),
            gammaToLinear(backdrop.b), 1.0f));
        _cameraComp->camera()->setFarClip(100.0f);
        _cameraComp->camera()->setFov(40.0f);
        _cameraComp->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_IMMEDIATE, LAYERID_UI});
        if (auto* controls = addOrbitControls(camera, Vector3(0.2f, 1.35f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-85.0f, 5.0f));
            controls->setZoomRange(Vector2(3.0f, 16.0f));
            controls->storeResetState();
        }

        // Post effects. The acrylic stand refracts the scene behind it.
        _cameraComp->setToneMapping(TONEMAP_NEUTRAL);
        _cameraComp->requestSceneColorMap(true);
        auto rendering = _cameraComp->rendering();
        rendering.bloomBlurLevel = 7;
        rendering.vignetteInner = 0.4f;
        rendering.vignetteOuter = 1.2f;
        _cameraComp->setRendering(rendering);
        // SMAA is an optional effect, added to the camera frame to be enabled when selected.
        _smaa = std::make_shared<SmaaEffect>();
        _cameraComp->addFrameEffect(_smaa);

        // Section caps: the back faces of the cut parts, drawn flat red, show through the cut
        // where the parts are solid. This needs closed meshes, and shows their inner surfaces,
        // not the cut plane.
        _capMaterial = std::make_shared<StandardMaterial>();
        _capMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _capMaterial->setEmissive(Color(0.62f, 0.05f, 0.04f, 1.0f));
        _capMaterial->setUseLighting(false);
        _capMaterial->setCullMode(CullMode::CULLFACE_FRONT);

        _renders = _engineNode->findComponents<RenderComponent>();
        _caps = new Entity();
        _caps->setName("section-caps");
        _caps->setEngine(engine());
        _capsRender = static_cast<RenderComponent*>(_caps->addComponent<RenderComponent>());
        _capsRender->setCastShadows(false);
        // A cap draws the mesh of a part, and its instances when it is instanced, on the
        // part's node.
        for (auto* render : _renders) {
            for (auto* meshInstance : render->meshInstances()) {
                auto cap = std::make_unique<MeshInstance>(meshInstance->mesh(), _capMaterial.get(),
                    meshInstance->node());
                const auto& instancing = meshInstance->instancingData();
                if (instancing.vertexBuffer && instancing.count > 0) {
                    cap->setInstancing(instancing.vertexBuffer, instancing.count);
                }
                cap->setCastShadow(false);
                _capsRender->addMeshInstance(std::move(cap));
            }
        }
        root()->addChild(_caps);

        applyEffects();
        updateCut();
        spdlog::info("Keys: C cut, [ ] opening, , . rotation, - = length, K caps, Space spin, A anti-aliasing, "
                     "B bloom, O ambient occlusion, V vignette");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_C: _cutEnabled = !_cutEnabled; break;
        case SDLK_LEFTBRACKET: _opening = std::max(0.0f, _opening - 10.0f); break;
        case SDLK_RIGHTBRACKET: _opening = std::min(180.0f, _opening + 10.0f); break;
        case SDLK_COMMA: _rotation = std::max(-180.0f, _rotation - 15.0f); break;
        case SDLK_PERIOD: _rotation = std::min(180.0f, _rotation + 15.0f); break;
        case SDLK_MINUS: _reveal = 1.0f; _length = std::max(0.0f, _length - 0.1f); break;
        case SDLK_EQUALS: _reveal = 1.0f; _length = std::min(1.0f, _length + 0.1f); break;
        case SDLK_K: _capsEnabled = !_capsEnabled; break;
        case SDLK_SPACE: _spin = !_spin; break;
        case SDLK_A: _antiAliasing = static_cast<AntiAliasing>((static_cast<int>(_antiAliasing) + 1) % 4); break;
        case SDLK_B: _bloom = !_bloom; break;
        case SDLK_O: _ssao = !_ssao; break;
        case SDLK_V: _vignette = !_vignette; break;
        default: return false;
        }
        applyEffects();
        updateCut();
        spdlog::info("cut {}, opening {}, rotation {}, length {:.2f}, caps {}, spin {}, aa {}, bloom {}, ssao {}, "
                     "vignette {}", _cutEnabled ? "on" : "off", _opening, _rotation, _length,
            _capsEnabled ? "on" : "off", _spin ? "on" : "off", kAntiAliasingNames[static_cast<size_t>(_antiAliasing)],
            _bloom ? "on" : "off", _ssao ? "on" : "off", _vignette ? "on" : "off");
        return true;
    }

    void update(const float dt) override
    {
        // On start, the cut runs from the inlet back to the exhaust.
        if (_reveal < 1.0f) {
            _reveal = std::min(1.0f, _reveal + dt / kRevealSeconds);
            _length = smootherstep(_reveal);
            updateCut();
        }

        // The spools turn slowly, the fan and its turbine slower than the core.
        if (_spin) {
            if (_lowSpool) {
                _lowSpool->rotateLocal(-40.0f * dt, 0.0f, 0.0f);
            }
            if (_highSpool) {
                _highSpool->rotateLocal(-70.0f * dt, 0.0f, 0.0f);
            }
        }
    }

private:
    void applyEffects()
    {
        auto rendering = _cameraComp->rendering();
        rendering.samples = _antiAliasing == AntiAliasing::Msaa ? 4 : 1;
        rendering.sharpness = _antiAliasing == AntiAliasing::Taa ? 0.5f : 0.0f;
        rendering.bloomIntensity = _bloom ? 0.02f : 0.0f;
        rendering.vignetteEnabled = _vignette;
        rendering.vignetteIntensity = 0.45f;
        _cameraComp->setRendering(rendering);

        auto taa = _cameraComp->taa();
        taa.enabled = _antiAliasing == AntiAliasing::Taa;
        _cameraComp->setTaa(taa);
        _smaa->setEnabled(_antiAliasing == AntiAliasing::Smaa);

        auto ssao = _cameraComp->ssao();
        ssao.enabled = _ssao;
        ssao.type = "combine";
        ssao.radius = 0.4f;
        ssao.samples = 12;
        ssao.scale = 0.5f;
        ssao.intensity = 0.5f;
        _cameraComp->setSsao(ssao);
    }

    // The cut: two planes through the engine axis remove a wedge, and in intersection mode only
    // what is behind all planes is clipped, so a third plane across the axis limits how far back
    // it runs.
    void updateCut()
    {
        const Vector3 axis = _engineNode->position();
        const float rotation = radians(_rotation);
        const float end = rotation + radians(_opening);
        std::array<Plane, 3> planes;
        planes[0].setFromPointNormal(axis, Vector3(0.0f, -std::cos(rotation), std::sin(rotation)));
        planes[1].setFromPointNormal(axis, Vector3(0.0f, std::cos(end), -std::sin(end)));
        const float x = axis.getX() + kInlet + (kExhaust - kInlet) * _length;
        planes[2].setFromPointNormal(Vector3(x, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));

        _caps->setEnabled(_cutEnabled && _capsEnabled);
        std::vector<RenderComponent*> renders = _renders;
        renders.push_back(_capsRender);
        for (auto* render : renders) {
            render->setClippingPlanes(_cutEnabled ? std::span<const Plane>(planes) : std::span<const Plane>());
            render->setClipIntersection(true);
        }
    }

    std::unique_ptr<Asset> _turbofanAsset;
    std::unique_ptr<Asset> _envAtlasAsset;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::shared_ptr<StandardMaterial> _capMaterial;
    std::shared_ptr<SmaaEffect> _smaa;
    CameraComponent* _cameraComp = nullptr;
    Entity* _engineNode = nullptr;
    GraphNode* _lowSpool = nullptr;
    GraphNode* _highSpool = nullptr;
    Entity* _caps = nullptr;
    RenderComponent* _capsRender = nullptr;
    std::vector<RenderComponent*> _renders;

    bool _cutEnabled = true;
    float _opening = 90.0f;
    float _rotation = 0.0f;
    float _length = 0.0f;
    bool _capsEnabled = true;
    bool _spin = true;
    float _reveal = 0.0f;
    AntiAliasing _antiAliasing = AntiAliasing::Msaa;
    bool _bloom = true;
    bool _ssao = true;
    bool _vignette = true;
};

VISUTWIN_EXAMPLE_MAIN(ClippingPlanesExample)
