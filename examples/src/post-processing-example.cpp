// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Post-processing compose-chain showcase (port of upstream graphics/post-processing).
//
// A statue hero on a floor, ringed by bright EMISSIVE orbiting orbs so BLOOM is
// obvious, all viewed through the full camera compose chain:
//   ACES tonemapping -> bloom -> vignette -> color grading (contrast/saturation)
//   -> color enhance (vibrance) -> fringing (chromatic aberration) -> a subtle DOF.
//
// Every effect is individually toggleable at runtime so the contribution of each
// stage is visible in isolation. Keys are logged at startup and on each change.
//
#include <cmath>
#include <memory>
#include <vector>

#include <framework/assets/asset.h>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class PostProcessingExample final: public ExampleApp
{
public:
    PostProcessingExample()
        : ExampleApp({.title = "Post-Processing Example", .width = 1024, .height = 768}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Post-Processing Example Started ***");

        // Skydome + IBL from the env atlas.
        scene()->setSkyboxMip(1);
        scene()->setExposure(1.6f);
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.15f, 0.15f, 0.18f);

        // Shadow-casting directional key light.
        auto* light = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 0.95f, 0.85f, 1.0f), 1.4f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(2048);
            lightComp->setShadowDistance(60.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        // Env atlas (skybox + specular/diffuse IBL).
        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        if (const auto envAtlasResource = _envAtlas->resource()) {
            scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
        } else {
            spdlog::warn("Failed to load environment atlas — continuing without IBL");
        }

        // Floor.
        auto floorMat = std::make_shared<StandardMaterial>();
        floorMat->setDiffuse(Color(0.12f, 0.12f, 0.14f, 1.0f));
        floorMat->setMetalness(0.0f);
        floorMat->setGloss(0.55f);
        _materials.push_back(floorMat);
        createPrimitive("plane", floorMat.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(40.0f, 1.0f, 40.0f));

        // Statue hero (DOF focal subject).
        _statue = std::make_unique<Asset>(
            "statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        if (const auto statueResource = _statue->resource()) {
            auto* statueEntity = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
            statueEntity->setLocalScale(0.5f, 0.5f, 0.5f);
            statueEntity->setLocalPosition(0.0f, 0.0f, 0.0f);
            root()->addChild(statueEntity);
        } else {
            spdlog::warn("Failed to load statue model — using a placeholder box hero");
            auto heroMat = std::make_shared<StandardMaterial>();
            heroMat->setDiffuse(Color(0.8f, 0.8f, 0.82f, 1.0f));
            heroMat->setMetalness(0.1f);
            heroMat->setGloss(0.6f);
            _materials.push_back(heroMat);
            createPrimitive("box", heroMat.get(), Vector3(0.0f, 3.0f, 0.0f), Vector3(3.0f, 6.0f, 3.0f));
        }

        // Bright EMISSIVE orbiting orbs — the bloom sources.
        const Color emissiveColors[] = {
            Color(1.0f, 0.15f, 0.1f, 1.0f),   // red
            Color(0.15f, 1.0f, 0.25f, 1.0f),  // green
            Color(0.2f, 0.35f, 1.0f, 1.0f),   // blue
            Color(1.0f, 0.75f, 0.1f, 1.0f),   // amber
            Color(0.9f, 0.15f, 1.0f, 1.0f),   // magenta
            Color(0.1f, 0.95f, 1.0f, 1.0f),   // cyan
        };
        constexpr int orbCount = 6;
        for (int i = 0; i < orbCount; ++i) {
            auto mat = std::make_shared<StandardMaterial>();
            mat->setDiffuse(Color(0.02f, 0.02f, 0.02f, 1.0f));
            mat->setMetalness(0.0f);
            mat->setGloss(0.2f);
            mat->setEmissive(emissiveColors[i]);
            mat->setEmissiveIntensity(6.0f); // > 1 pushes into bloom range
            _materials.push_back(mat);

            Orb orb;
            orb.radius = 9.0f;
            orb.speed = 0.35f + 0.05f * static_cast<float>(i);
            orb.phase = static_cast<float>(i) * (2.0f * 3.14159265f / orbCount);
            orb.height = 3.0f + 1.5f * std::sin(static_cast<float>(i));
            orb.entity = createPrimitive("sphere", mat.get(),
                Vector3(orb.radius, orb.height, 0.0f), Vector3(0.9f, 0.9f, 0.9f));
            _orbs.push_back(orb);
        }

        constexpr float sceneRadius = 12.0f;

        // Camera.
        auto* camera = createCamera(_focusPoint + Vector3(0.0f, 4.0f, kOrbitDistance));
        _cameraComp = camera->findComponent<CameraComponent>();

        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->camera()->setNearClip(0.5f);
            _cameraComp->camera()->setFarClip(200.0f);
            _cameraComp->camera()->setFov(55.0f);
        }

        // ── Configure the full compose chain ──────────────────────────────────
        if (_cameraComp) {
            // TAA — steadies bloom/fringing under motion (subtle, on by default).
            auto taa = _cameraComp->taa();
            taa.enabled = true;
            taa.highQuality = true;
            taa.jitter = 1.0f;
            _cameraComp->setTaa(taa);

            auto rendering = _cameraComp->rendering();
            rendering.toneMapping = TONEMAP_ACES;
            rendering.sharpness = 0.4f;

            // Bloom — makes the emissive orbs glow.
            rendering.bloomIntensity = 0.04f;

            // Vignette — darken the frame edges.
            rendering.vignetteEnabled = true;
            rendering.vignetteInner = 0.35f;
            rendering.vignetteOuter = 1.1f;
            rendering.vignetteCurvature = 0.5f;
            rendering.vignetteIntensity = 0.55f;

            // Color grading — a touch more contrast + saturation, cool tint.
            rendering.gradingEnabled = true;
            rendering.gradingBrightness = 1.0f;
            rendering.gradingContrast = 1.08f;
            rendering.gradingSaturation = 1.15f;
            rendering.gradingTint[0] = 0.98f;
            rendering.gradingTint[1] = 1.0f;
            rendering.gradingTint[2] = 1.05f;

            // Color enhance — lift vibrance a little.
            rendering.colorEnhanceVibrance = 0.25f;

            // Fringing — subtle chromatic aberration.
            rendering.fringingIntensity = 12.0f;

            _cameraComp->setRendering(rendering);

            // Depth of field — focus the statue, blur the orbiting orbs.
            auto dof = _cameraComp->dof();
            dof.enabled = true;
            dof.nearBlur = true;
            dof.focusDistance = kOrbitDistance;
            dof.focusRange = 8.0f;
            dof.blurRadius = 3.0f;
            dof.blurRings = 4;
            dof.blurRingPoints = 5;
            dof.highQuality = true;
            _cameraComp->setDof(dof);
        }

        _controls = addOrbitControls(camera, _focusPoint);
        _controls->setAutoFarClip(true);
        _controls->setMoveSpeed(2 * sceneRadius);
        _controls->setMoveFastSpeed(4 * sceneRadius);
        _controls->setMoveSlowSpeed(sceneRadius);
        _controls->setOrbitDistance(kOrbitDistance);
        _controls->storeResetState();

        spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
        spdlog::info("Compose toggles:");
        spdlog::info("  B  bloom on/off");
        spdlog::info("  V  vignette on/off");
        spdlog::info("  G  color grading on/off");
        spdlog::info("  C  color enhance (vibrance) on/off");
        spdlog::info("  X  fringing (chromatic aberration) on/off");
        spdlog::info("  D  depth of field on/off");
        spdlog::info("  T  TAA on/off");
        spdlog::info("  M  cycle tonemapping (ACES -> NONE -> LINEAR)");
        spdlog::info("  S  cycle sharpness (0 -> 0.25 -> 0.5 -> 0.75 -> 1.0)");
        spdlog::info("  0  disable ALL compose effects   1  restore ALL");
        spdlog::info("  ESC quit");
        logState("init");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }

        auto r = _cameraComp->rendering();
        switch (event.key.key) {
        case SDLK_B:
            r.bloomIntensity = r.bloomIntensity > 0.0f ? 0.0f : 0.04f;
            _cameraComp->setRendering(r);
            logState("bloom");
            return true;
        case SDLK_V:
            r.vignetteEnabled = !r.vignetteEnabled;
            _cameraComp->setRendering(r);
            logState("vignette");
            return true;
        case SDLK_G:
            r.gradingEnabled = !r.gradingEnabled;
            _cameraComp->setRendering(r);
            logState("grading");
            return true;
        case SDLK_C:
            r.colorEnhanceVibrance = r.colorEnhanceVibrance > 0.0f ? 0.0f : 0.25f;
            _cameraComp->setRendering(r);
            logState("color-enhance");
            return true;
        case SDLK_X:
            r.fringingIntensity = r.fringingIntensity > 0.0f ? 0.0f : 12.0f;
            _cameraComp->setRendering(r);
            logState("fringing");
            return true;
        case SDLK_D: {
            auto d = _cameraComp->dof();
            d.enabled = !d.enabled;
            _cameraComp->setDof(d);
            logState("dof");
            return true;
        }
        case SDLK_T: {
            auto taa = _cameraComp->taa();
            taa.enabled = !taa.enabled;
            _cameraComp->setTaa(taa);
            spdlog::info("[taa] {}", taa.enabled ? "ON" : "OFF");
            return true;
        }
        case SDLK_M:
            if (r.toneMapping == TONEMAP_ACES) r.toneMapping = TONEMAP_NONE;
            else if (r.toneMapping == TONEMAP_NONE) r.toneMapping = TONEMAP_LINEAR;
            else r.toneMapping = TONEMAP_ACES;
            _cameraComp->setRendering(r);
            scene()->setToneMapping(r.toneMapping);
            logState("tonemap");
            return true;
        case SDLK_S:
            if (r.sharpness < 0.125f) r.sharpness = 0.25f;
            else if (r.sharpness < 0.375f) r.sharpness = 0.5f;
            else if (r.sharpness < 0.625f) r.sharpness = 0.75f;
            else if (r.sharpness < 0.875f) r.sharpness = 1.0f;
            else r.sharpness = 0.0f;
            _cameraComp->setRendering(r);
            logState("sharpness");
            return true;
        case SDLK_0: {
            r.bloomIntensity = 0.0f;
            r.vignetteEnabled = false;
            r.gradingEnabled = false;
            r.colorEnhanceVibrance = 0.0f;
            r.fringingIntensity = 0.0f;
            r.sharpness = 0.0f;
            _cameraComp->setRendering(r);
            auto d = _cameraComp->dof();
            d.enabled = false;
            _cameraComp->setDof(d);
            logState("all-off");
            return true;
        }
        case SDLK_1: {
            r.bloomIntensity = 0.04f;
            r.vignetteEnabled = true;
            r.gradingEnabled = true;
            r.colorEnhanceVibrance = 0.25f;
            r.fringingIntensity = 12.0f;
            r.sharpness = 0.4f;
            _cameraComp->setRendering(r);
            auto d = _cameraComp->dof();
            d.enabled = true;
            _cameraComp->setDof(d);
            logState("all-on");
            return true;
        }
        case SDLK_F:
            if (_controls) {
                _controls->focus(_focusPoint, kOrbitDistance);
            }
            return true;
        default:
            return false;
        }
    }

    void update(const float dt) override
    {
        _totalTime += dt;

        // Orbit the emissive orbs around the hero.
        for (const auto& orb : _orbs) {
            if (!orb.entity) { continue; }
            const float a = orb.phase + _totalTime * orb.speed * 2.0f * 3.14159265f;
            orb.entity->setLocalPosition(
                orb.radius * std::cos(a),
                orb.height,
                orb.radius * std::sin(a));
        }
    }

    void destroy() override
    {
        spdlog::info("*** Post-Processing Example Finished ***");
    }

private:
    struct Orb
    {
        Entity* entity = nullptr;
        float radius = 0.0f;
        float speed = 0.0f;
        float phase = 0.0f;
        float height = 0.0f;
    };

    void logState(const char* reason) const
    {
        if (!_cameraComp) { return; }
        const auto& r = _cameraComp->rendering();
        const auto& d = _cameraComp->dof();
        spdlog::info(
            "[{}] tonemap={} bloom={:.3f} vignette={} grading={} vibrance={:.2f} fringing={:.1f} dof={} sharpness={:.2f}",
            reason,
            r.toneMapping == TONEMAP_ACES ? "ACES" : (r.toneMapping == TONEMAP_NONE ? "NONE" : "LINEAR"),
            r.bloomIntensity,
            r.vignetteEnabled ? "ON" : "OFF",
            r.gradingEnabled ? "ON" : "OFF",
            r.colorEnhanceVibrance,
            r.fringingIntensity,
            d.enabled ? "ON" : "OFF",
            r.sharpness);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _statue;
    // Materials kept alive for the lifetime of the program.
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Orb> _orbs;

    CameraComponent* _cameraComp = nullptr;
    CameraControls* _controls = nullptr;

    const Vector3 _focusPoint{0.0f, 3.0f, 0.0f};
    static constexpr float kOrbitDistance = 26.0f;
    float _totalTime = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(PostProcessingExample)
