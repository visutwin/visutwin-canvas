// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Depth-of-field showcase — port of upstream graphics/depth-of-field.
//
// An apartment interior lit purely by the helipad environment atlas, with an
// Egyptian cat statue on the floor at the focus plane and an emissive neon "love"
// sign blooming on the far wall. Bokeh DOF keeps the cat razor-sharp while the
// near furniture and the far end of the room dissolve into blur.
//
// Upstream drives DOF from a control panel; this port maps the same settings onto
// keys (listed at startup), all going through CameraComponent::dof().
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <core/shape/boundingBox.h>
#include <framework/assets/asset.h>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// World-space AABB of every mesh instance under `entity`.
BoundingBox entityAabb(Entity* entity)
{
    BoundingBox bbox;
    bbox.setCenter(0.0f, 0.0f, 0.0f);
    bbox.setHalfExtents(0.0f, 0.0f, 0.0f);
    if (!entity) {
        return bbox;
    }

    bool any = false;
    for (auto* render : entity->findComponents<RenderComponent>()) {
        for (auto* meshInstance : render->meshInstances()) {
            if (!meshInstance) {
                continue;
            }
            if (!any) {
                bbox = meshInstance->aabb();
                any = true;
            } else {
                bbox.add(meshInstance->aabb());
            }
        }
    }

    if (!any) {
        bbox.setCenter(entity->position());
        bbox.setHalfExtents(0.5f, 0.5f, 0.5f);
    }
    return bbox;
}

// Runs `fn` for every material under `entity`.
template <typename Fn>
void forEachMaterial(Entity* entity, Fn&& fn)
{
    if (!entity) {
        return;
    }
    for (auto* render : entity->findComponents<RenderComponent>()) {
        for (auto* meshInstance : render->meshInstances()) {
            if (auto* material = meshInstance ? meshInstance->material() : nullptr) {
                fn(material);
            }
        }
    }
}

class DepthOfFieldExample final: public ExampleApp
{
public:
    DepthOfFieldExample()
        : ExampleApp({.title = "Depth-of-Field Example", .width = 1024, .height = 768}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Depth-of-Field Example Started ***");

        // Environment atlas — the scene's only light source (IBL + skydome).
        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _apartment = std::make_unique<Asset>(
            "apartment", AssetType::CONTAINER, assetPath("models/apartment.glb"));
        _love = std::make_unique<Asset>(
            "love", AssetType::CONTAINER, assetPath("models/love.glb"));
        _cat = std::make_unique<Asset>(
            "cat", AssetType::CONTAINER, assetPath("models/cat.glb"));

        // Skydome + IBL from the env atlas — there is no other light in the scene.
        const auto envAtlasResource = _envAtlas->resource();
        if (!envAtlasResource) {
            spdlog::error("Failed to load the environment atlas — the scene has no other light");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));
        scene()->setExposure(1.2f);

        // ── Apartment interior ──────────────────────────────────────────────────
        const auto apartmentResource = _apartment->resource();
        if (!apartmentResource) {
            spdlog::error("Failed to load models/apartment.glb");
            return false;
        }
        auto* apartmentEntity = std::get<ContainerResource*>(*apartmentResource)->instantiateRenderEntity();
        apartmentEntity->setLocalScale(30.0f, 30.0f, 30.0f);
        root()->addChild(apartmentEntity);

        // ── Neon "love" sign on the far wall ────────────────────────────────────
        const auto loveResource = _love->resource();
        if (!loveResource) {
            spdlog::error("Failed to load models/love.glb");
            return false;
        }
        auto* loveEntity = std::get<ContainerResource*>(*loveResource)->instantiateRenderEntity();
        loveEntity->setLocalPosition(-335.0f, 180.0f, 0.0f);
        loveEntity->setLocalScale(130.0f, 130.0f, 130.0f);
        root()->addChild(loveEntity);

        // Make the neon tube emissive enough to bloom.
        if (auto* neon = dynamic_cast<Entity*>(loveEntity->findByName("s.0009_Standard_FF00BB_0"))) {
            forEachMaterial(neon, [](Material* material) {
                if (auto* standard = dynamic_cast<StandardMaterial*>(material)) {
                    standard->setEmissiveIntensity(200.0f);
                }
            });
        } else {
            spdlog::warn("Neon mesh 's.0009_Standard_FF00BB_0' not found — the sign will not bloom");
        }

        // The sign's glass uses transmission; dynamic refraction is not wanted here.
        forEachMaterial(loveEntity, [](Material* material) {
            if (auto* standard = dynamic_cast<StandardMaterial*>(material)) {
                standard->setUseDynamicRefraction(false);
            }
        });

        // ── Cat statue: the focal object ────────────────────────────────────────
        const auto catResource = _cat->resource();
        if (!catResource) {
            spdlog::error("Failed to load models/cat.glb");
            return false;
        }
        auto* catEntity = std::get<ContainerResource*>(*catResource)->instantiateRenderEntity();
        catEntity->setLocalPosition(-80.0f, 80.0f, -20.0f);
        catEntity->setLocalScale(80.0f, 80.0f, 80.0f);
        root()->addChild(catEntity);

        // ── Camera ──────────────────────────────────────────────────────────────
        auto* camera = createCamera(Vector3(-50.0f, 100.0f, 220.0f));
        _cameraComp = camera->findComponent<CameraComponent>();

        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->camera()->setNearClip(0.1f);
            _cameraComp->camera()->setFarClip(1500.0f);
            _cameraComp->camera()->setFov(80.0f);
        }

        if (_cameraComp) {
            // Bokeh depth of field, upstream's defaults.
            auto dof = _cameraComp->dof();
            dof.enabled = true;
            dof.nearBlur = true;               // blur nearer-than-focus too
            dof.focusDistance = 200.0f;        // sharp plane distance from camera
            dof.focusRange = 100.0f;           // depth window kept in focus
            dof.blurRadius = 5.0f;             // bokeh circle-of-confusion size
            dof.blurRings = 4;                 // concentric bokeh sample rings
            dof.blurRingPoints = 5;            // samples per ring
            dof.highQuality = true;
            _cameraComp->setDof(dof);

            auto rendering = _cameraComp->rendering();
            rendering.toneMapping = TONEMAP_ACES;
            rendering.samples = 4;             // 4x MSAA on the offscreen scene target
            rendering.bloomIntensity = 0.03f;
            rendering.bloomBlurLevel = 7;      // tighter glow than the 16-level default
            rendering.vignetteEnabled = true;
            rendering.vignetteInner = 0.5f;
            rendering.vignetteOuter = 1.0f;
            rendering.vignetteCurvature = 0.5f;
            rendering.vignetteIntensity = 0.5f;
            _cameraComp->setRendering(rendering);
        }

        // Upstream's orbit camera aims at the focus entity's AABB centre on initialize
        // (its own lookAt(0, 0, 100) never survives), keeping the camera position and
        // deriving the orbit distance from it — which is exactly what setFocusPoint does.
        const BoundingBox catBbox = entityAabb(catEntity);
        _focusPoint = catBbox.center();
        const float sceneRadius = std::max(catBbox.halfExtents().length(), 1.0f);

        _controls = addOrbitControls(camera, _focusPoint);
        _controls->setMoveSpeed(2 * sceneRadius);
        _controls->setMoveFastSpeed(4 * sceneRadius);
        _controls->setMoveSlowSpeed(sceneRadius);
        _controls->storeResetState();
        _orbitDistance = camera->position().distance(_focusPoint);

        spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
        spdlog::info("Depth-of-field controls:");
        spdlog::info("  D        toggle DOF on/off");
        spdlog::info("  [        move focus distance CLOSER (-10)");
        spdlog::info("  ]        move focus distance FARTHER (+10)");
        spdlog::info("  , / .    narrow / widen focus range (+/-5)");
        spdlog::info("  - / =    smaller / larger blur radius (+/-1)");
        spdlog::info("  N        toggle near-blur");
        spdlog::info("  Q        toggle high-quality bokeh");
        spdlog::info("  ESC      quit");
        logDof("init");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }

        auto d = _cameraComp->dof();
        switch (event.key.key) {
        case SDLK_D:
            d.enabled = !d.enabled;
            _cameraComp->setDof(d);
            logDof("toggle");
            return true;
        case SDLK_LEFTBRACKET:
            d.focusDistance = std::max(1.0f, d.focusDistance - 10.0f);
            _cameraComp->setDof(d);
            logDof("focus-closer");
            return true;
        case SDLK_RIGHTBRACKET:
            d.focusDistance = std::min(1400.0f, d.focusDistance + 10.0f);
            _cameraComp->setDof(d);
            logDof("focus-farther");
            return true;
        case SDLK_COMMA:
            d.focusRange = std::max(1.0f, d.focusRange - 5.0f);
            _cameraComp->setDof(d);
            logDof("range-narrow");
            return true;
        case SDLK_PERIOD:
            d.focusRange = std::min(500.0f, d.focusRange + 5.0f);
            _cameraComp->setDof(d);
            logDof("range-widen");
            return true;
        case SDLK_MINUS:
            d.blurRadius = std::max(1.0f, d.blurRadius - 1.0f);
            _cameraComp->setDof(d);
            logDof("blur-smaller");
            return true;
        case SDLK_EQUALS:
            d.blurRadius = std::min(20.0f, d.blurRadius + 1.0f);
            _cameraComp->setDof(d);
            logDof("blur-larger");
            return true;
        case SDLK_N:
            d.nearBlur = !d.nearBlur;
            _cameraComp->setDof(d);
            logDof("near-blur");
            return true;
        case SDLK_Q:
            d.highQuality = !d.highQuality;
            _cameraComp->setDof(d);
            logDof("high-quality");
            return true;
        case SDLK_F:
            if (_controls) {
                _controls->focus(_focusPoint, _orbitDistance);
            }
            return true;
        default:
            return false;
        }
    }

    void destroy() override
    {
        spdlog::info("*** Depth-of-Field Example Finished ***");
    }

private:
    void logDof(const char* reason) const
    {
        if (!_cameraComp) {
            return;
        }
        const auto& d = _cameraComp->dof();
        spdlog::info(
            "[DOF {}] enabled={} nearBlur={} focusDistance={:.1f} focusRange={:.1f} "
            "blurRadius={:.1f} rings={} ringPoints={} highQuality={}",
            reason,
            d.enabled ? "ON" : "OFF",
            d.nearBlur ? "ON" : "OFF",
            d.focusDistance, d.focusRange, d.blurRadius,
            d.blurRings, d.blurRingPoints,
            d.highQuality ? "ON" : "OFF");
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _apartment;
    std::unique_ptr<Asset> _love;
    std::unique_ptr<Asset> _cat;

    CameraComponent* _cameraComp = nullptr;
    CameraControls* _controls = nullptr;
    Vector3 _focusPoint;
    float _orbitDistance = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(DepthOfFieldExample)
