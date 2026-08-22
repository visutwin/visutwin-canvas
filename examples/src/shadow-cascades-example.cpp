// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Shadow cascades example — a low-poly terrain under a low sun, with cascaded
// shadow maps and volumetric fog. Keys retune the cascade count, distribution,
// blend and shadow resolution at runtime, and a MiniStats HUD shows the cost.
//
#include <algorithm>
#include <memory>

#include <framework/assets/asset.h>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/extras/miniStats.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

// The ImGui overlay is implemented with imgui_impl_metal, so it is Metal-only.
#ifdef VISUTWIN_HAS_METAL
#include "platform/graphics/metal/metalGraphicsDevice.h"
#include "viz/overlay/imguiOverlay.h"
#endif

using namespace visutwin::canvas;

class ShadowCascadesExample final: public ExampleApp
{
public:
    ShadowCascadesExample()
        : ExampleApp({.title = "Shadow Cascades Example", .width = 1100, .height = 750}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Shadow Cascades Example Started ***");

        // ImGui overlay + MiniStats HUD. The HUD hooks the engine's "postrender" event, so
        // nothing else has to be called per frame.
        //
        // The overlay only works on a Metal device. Both backends can be compiled in and
        // picked at runtime (VISUTWIN_BACKEND), and an unchecked cast here used to hand
        // ImGui_ImplMetal_Init a Vulkan device — a segfault at startup that made this
        // example unusable on Vulkan.
#ifdef VISUTWIN_HAS_METAL
        auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(device().get());
        if (metalDevice) {
            _overlay = std::make_unique<ImGuiOverlay>();
            _overlay->init(metalDevice, window());
        } else {
            spdlog::warn("ImGui overlay and the MiniStats HUD need a Metal device; "
                "skipping them on this backend.");
        }
        _miniStats = std::make_unique<MiniStats>(enginePtr(), _overlay.get());
#else
        spdlog::warn("ImGui overlay and the MiniStats HUD need a Metal device; "
            "skipping them on this backend.");
#endif

        // setup skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(1.2f);
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.25f, 0.28f, 0.35f);

        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        const auto envAtlasResource = _envAtlas->resource();
        if (!envAtlasResource) {
            spdlog::error("Failed to load environment atlas texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

        // -----------------------------------------------------------------------
        // Instantiate the low-poly terrain model (mirrors upstream shadow-cascades).
        // The GLB contains ground, trees and static clouds; scaled up so cascades
        // span it at varying distance.
        // -----------------------------------------------------------------------
        _terrainAsset = std::make_unique<Asset>(
            "terrain", AssetType::CONTAINER, assetPath("models/terrain.glb"));
        const auto terrainResource = _terrainAsset->resource();
        if (!terrainResource || !std::holds_alternative<ContainerResource*>(*terrainResource)) {
            spdlog::error("Failed to load terrain.glb");
            return false;
        }
        if (auto* terrainEntity = std::get<ContainerResource*>(*terrainResource)->instantiateRenderEntity()) {
            terrainEntity->setEngine(engine());
            terrainEntity->setLocalScale(30.0f, 30.0f, 30.0f);
            root()->addChild(terrainEntity);
        }

        // -----------------------------------------------------------------------
        // Directional light with cascaded shadows. Low sun angle for long dramatic
        // shadows.
        // -----------------------------------------------------------------------
        auto* dirLight = createDirectionalLight(Vector3(25.0f, 330.0f, 0.0f),
            Color(1.0f, 0.95f, 0.85f), 1.2f, true);
        _lightComp = dirLight->findComponent<LightComponent>();
        if (_lightComp) {
            _lightComp->setShadowBias(0.05f);
            _lightComp->setShadowNormalBias(0.5f);
            _lightComp->setShadowDistance(1000.0f);

            _lightComp->setNumCascades(_numCascades);
            _lightComp->setShadowResolution(_shadowResolution);
            _lightComp->setCascadeDistribution(_cascadeDistribution);
            _lightComp->setCascadeBlend(_cascadeBlend);
        }

        // -----------------------------------------------------------------------
        // Camera
        // -----------------------------------------------------------------------
        auto* camera = createCamera(Vector3(300.0f, 160.0f, 25.0f));
        if (auto* cameraComp = camera->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            // upstream uses a light-grey clear behind the terrain.
            cameraComp->camera()->setClearColor(Color(0.9f, 0.9f, 0.9f, 1.0f));
            cameraComp->camera()->setFarClip(2000.0f);

            auto rendering = cameraComp->rendering();
            rendering.toneMapping = TONEMAP_ACES;
            cameraComp->setRendering(rendering);

            // Volumetric fog: the low sun and cascaded shadows give pronounced light shafts.
            auto fog = cameraComp->volumetricFog();
            fog.enabled = true;
            fog.density = 0.0025f;
            fog.heightBase = 0.0f;
            fog.heightFalloff = 0.010f;
            fog.anisotropy = 0.75f;      // strong forward scattering, so the sun glows through
            fog.intensity = 3.0f;
            // The ambient term has to be in the same ballpark as the sky it replaces: extinction
            // removes the background light, and only in-scattering puts light back.
            fog.ambientColor[0] = 0.55f;
            fog.ambientColor[1] = 0.62f;
            fog.ambientColor[2] = 0.75f;
            fog.ambientIntensity = 0.45f;
            fog.maxDistance = 900.0f;
            fog.steps = 32;
            fog.scale = 0.5f;
            cameraComp->setVolumetricFog(fog);
        }

        _controls = addOrbitControls(camera, kFocusPoint);
        _controls->setAutoFarClip(true);
        _controls->setMoveSpeed(150.0f);
        _controls->setMoveFastSpeed(400.0f);
        _controls->setMoveSlowSpeed(40.0f);
        _controls->setOrbitDistance(kOrbitDistance);
        _controls->storeResetState();

        spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
        spdlog::info("CSM controls:");
        spdlog::info("  1-4: set cascade count");
        spdlog::info("  D/C: increase/decrease cascade distribution");
        spdlog::info("  B/V: increase/decrease cascade blend");
        spdlog::info("  +/-: increase/decrease shadow resolution");
        logCascadeState("init");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
#ifdef VISUTWIN_HAS_METAL
        if (_overlay) {
            _overlay->processEvent(event);
        }
#endif
        if (event.type != SDL_EVENT_KEY_DOWN || !_lightComp) {
            return false;
        }

        switch (event.key.key) {
        // Cascade count: 1-4 keys
        case SDLK_1:
        case SDLK_2:
        case SDLK_3:
        case SDLK_4:
            _numCascades = static_cast<int>(event.key.key - SDLK_1) + 1;
            _lightComp->setNumCascades(_numCascades);
            logCascadeState("count");
            return true;

        // Cascade distribution: D increase, C decrease
        case SDLK_D:
            _cascadeDistribution = std::min(1.0f, _cascadeDistribution + 0.05f);
            _lightComp->setCascadeDistribution(_cascadeDistribution);
            logCascadeState("distribution");
            return true;
        case SDLK_C:
            _cascadeDistribution = std::max(0.0f, _cascadeDistribution - 0.05f);
            _lightComp->setCascadeDistribution(_cascadeDistribution);
            logCascadeState("distribution");
            return true;

        // Cascade blend: B increase, V decrease
        case SDLK_B:
            _cascadeBlend = std::min(0.2f, _cascadeBlend + 0.01f);
            _lightComp->setCascadeBlend(_cascadeBlend);
            logCascadeState("blend");
            return true;
        case SDLK_V:
            _cascadeBlend = std::max(0.0f, _cascadeBlend - 0.01f);
            _lightComp->setCascadeBlend(_cascadeBlend);
            logCascadeState("blend");
            return true;

        // Shadow resolution: +/- keys
        case SDLK_EQUALS:
            _shadowResolution = std::min(4096, _shadowResolution * 2);
            _lightComp->setShadowResolution(_shadowResolution);
            logCascadeState("resolution");
            return true;
        case SDLK_MINUS:
            _shadowResolution = std::max(256, _shadowResolution / 2);
            _lightComp->setShadowResolution(_shadowResolution);
            logCascadeState("resolution");
            return true;

        case SDLK_F:
            if (_controls) {
                _controls->focus(kFocusPoint, kOrbitDistance);
            }
            return true;

        default:
            return false;
        }
    }

    void destroy() override
    {
        // Both hook engine events and hold a Metal device, so they go before the
        // engine does.
#ifdef VISUTWIN_HAS_METAL
        _miniStats.reset();
        _overlay.reset();
#endif
        spdlog::info("*** Shadow Cascades Example Finished ***");
    }

private:
    void logCascadeState(const char* reason) const
    {
        spdlog::info("CSM {}: cascades={}, resolution={}, distribution={:.2f}, blend={:.2f}",
            reason, _numCascades, _shadowResolution, _cascadeDistribution, _cascadeBlend);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _terrainAsset;

#ifdef VISUTWIN_HAS_METAL
    std::unique_ptr<ImGuiOverlay> _overlay;
    std::unique_ptr<MiniStats> _miniStats;
#endif

    LightComponent* _lightComp = nullptr;
    CameraControls* _controls = nullptr;

    // Cascade settings state
    int _numCascades = 4;
    float _cascadeDistribution = 0.5f;
    float _cascadeBlend = 5.0f;
    int _shadowResolution = 2048;

    const Vector3 kFocusPoint{0.0f, 40.0f, 0.0f};
    static constexpr float kOrbitDistance = 470.0f;
};

VISUTWIN_EXAMPLE_MAIN(ShadowCascadesExample)
