// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// SSAO showcase, da Vinci workshop variant: the workshop model and a Leonardo bust
// under a museum spotlight, both auto-scaled to a common size, with the full colour
// finishing chain (fringing, grading, colour enhance, 3D LUT) on top of the SSAO.
// Every SSAO parameter is bound to a key so its effect can be isolated at runtime.
//
#include <algorithm>
#include <cmath>
#include <memory>

#include <core/shape/boundingBox.h>
#include <framework/assets/asset.h>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "core/math/quaternion.h"
#include "platform/graphics/depthState.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

class AmbientOcclusionDavinciExample final: public ExampleApp
{
public:
    AmbientOcclusionDavinciExample()
        : ExampleApp({.title = "Ambient Occlusion Example"}) {}

protected:
    bool create() override
    {
        spdlog::info("*** Ambient Occlusion Example Started ***");

        // setup skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(1.5f);
        scene()->setToneMapping(TONEMAP_NEUTRAL);

        _envAtlas = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        _laboratory = std::make_unique<Asset>(
            "laboratory", AssetType::CONTAINER, assetPath("models/da_vinci_workshop.glb"));
        _leonardoBust = std::make_unique<Asset>(
            "leonardo", AssetType::CONTAINER, assetPath("models/leonardo_da_vinci.glb"));
        // 3D color LUT: 256x16 Unreal-format strip (teal-orange look) — no mipmaps.
        _colorLutAsset = std::make_unique<Asset>(
            "lut-teal-orange",
            AssetType::TEXTURE,
            assetPath("textures/lut-teal-orange.tga"),
            AssetData{ .mipmaps = false }
        );

        const auto envAtlasResource = _envAtlas->resource();
        if (!envAtlasResource) {
            spdlog::error("Failed to load environment atlas texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*envAtlasResource));

        // create laboratory entity — auto-scale to fit scene
        const auto labResource = _laboratory->resource();
        if (!labResource) {
            spdlog::error("Failed to load laboratory model");
            return false;
        }
        auto* labEntity = std::get<ContainerResource*>(*labResource)->instantiateRenderEntity();
        root()->addChild(labEntity);

        // Normalize model so its longest extent is ~100 units
        {
            const auto bbox = entityBounds(labEntity);
            const auto& he = bbox.halfExtents();
            const auto& ct = bbox.center();
            const float maxExtent = std::max({he.getX(), he.getY(), he.getZ()}) * 2.0f;
            if (maxExtent > 0.001f) {
                const float s = 100.0f / maxExtent;
                // Compose with the model's own root scale — the AABB was measured with it.
                const float s0 = labEntity->localScale().getX();
                labEntity->setLocalScale(s0 * s, s0 * s, s0 * s);
                labEntity->setLocalPosition(
                    -ct.getX() * s,
                    -ct.getY() * s + he.getY() * s + (-40.0f),
                    -ct.getZ() * s);
                spdlog::info("Laboratory model: extent={:.1f}, scale={:.3f}", maxExtent, s);
            }
        }

        // set up materials — enable shadows, disable baked AO, disable blending
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) continue;
            auto* owner = render->entity();
            if (owner != labEntity && !owner->isDescendantOf(labEntity)) continue;

            render->setCastShadows(true);
            render->setReceiveShadows(true);

            for (auto* mi : render->meshInstances()) {
                if (!mi || !mi->material()) continue;
                auto* mat = dynamic_cast<StandardMaterial*>(mi->material());
                if (!mat) continue;
                // disable baked AO map — we want SSAO only
                mat->setAoMap(nullptr);
                // disable blending / enable depth writes
                mat->setTransparent(false);
                mat->setDepthState(std::make_shared<DepthState>());
            }
        }

        // add Leonardo da Vinci bust next to the workshop
        Entity* leoEntity = nullptr;
        {
            const auto leoResource = _leonardoBust->resource();
            if (leoResource) {
                leoEntity = std::get<ContainerResource*>(*leoResource)->instantiateRenderEntity();
                root()->addChild(leoEntity);

                // Scale bust to same normalized size as the workshop (100 units)
                const auto leoBbox = entityBounds(leoEntity);
                const auto& leoHe = leoBbox.halfExtents();
                const auto& leoCt = leoBbox.center();
                const float leoMaxExtent = std::max({leoHe.getX(), leoHe.getY(), leoHe.getZ()}) * 2.0f;
                if (leoMaxExtent > 0.001f) {
                    const float leoScale = 150.0f / leoMaxExtent;
                    const float leoScale0 = leoEntity->localScale().getX();
                    leoEntity->setLocalScale(leoScale0 * leoScale, leoScale0 * leoScale, leoScale0 * leoScale);

                    // Place to the right of the workshop, sitting on the ground plane
                    const auto labBbox = entityBounds(labEntity);
                    const float offsetX = labBbox.center().getX() + labBbox.halfExtents().getX() + 100.0f;
                    leoEntity->setLocalPosition(
                        offsetX - leoCt.getX() * leoScale,
                        -leoCt.getY() * leoScale + leoHe.getY() * leoScale + (-40.0f),
                        -leoCt.getZ() * leoScale);

                    // Rotate 45 degrees to face toward the Mona Lisa portrait. Compose with
                    // the model's own root orientation (instantiateRenderEntity returns the
                    // glTF root node itself for single-root scenes) instead of replacing it.
                    leoEntity->setRotation(Quaternion::fromEulerAngles(0.0f, 135.0f, 0.0f) *
                                           leoEntity->rotation());

                    spdlog::info("Leonardo bust: extent={:.1f}, scale={:.3f}", leoMaxExtent, leoScale);
                }

                // Enable shadows on the bust
                for (auto* render : RenderComponent::instances()) {
                    if (!render || !render->entity()) continue;
                    auto* owner = render->entity();
                    if (owner != leoEntity && !owner->isDescendantOf(leoEntity)) continue;
                    render->setCastShadows(true);
                    render->setReceiveShadows(true);
                }
            } else {
                spdlog::warn("Leonardo bust model not found — skipping");
            }
        }

        // Center both objects on the ground plane (X=0, Z=0)
        {
            auto combined = entityBounds(labEntity);
            if (leoEntity) combined.add(entityBounds(leoEntity));
            const float shiftX = -combined.center().getX();
            const float shiftZ = -combined.center().getZ();
            if (std::abs(shiftX) > 0.01f || std::abs(shiftZ) > 0.01f) {
                auto lp = labEntity->position();
                labEntity->setLocalPosition(lp.getX() + shiftX, lp.getY(), lp.getZ() + shiftZ);
                if (leoEntity) {
                    auto leoP = leoEntity->position();
                    leoEntity->setLocalPosition(leoP.getX() + shiftX, leoP.getY(), leoP.getZ() + shiftZ);
                }
            }
        }

        // add fill point lights around the model
        {
            const auto bbox = entityBounds(labEntity);
            const auto& ct = bbox.center();
            const float r = bbox.halfExtents().length() * 0.6f;
            const float h = ct.getY() + bbox.halfExtents().getY() * 0.5f;

            struct LightDef { float dx; float dz; Color color; };
            const LightDef fills[] = {
                { -r,  r, Color(1.0f, 0.85f, 0.6f) },
                {  r, -r, Color(0.6f, 0.8f, 1.0f)  },
            };
            for (auto& def : fills) {
                auto* pl = new Entity();
                pl->setEngine(engine());
                if (auto* lc = static_cast<LightComponent*>(pl->addComponent<LightComponent>())) {
                    lc->setType(LightType::LIGHTTYPE_OMNI);
                    lc->setColor(def.color);
                    lc->setIntensity(1.0f);
                    lc->setRange(r * 4.0f);
                    lc->setCastShadows(false);
                }
                pl->setLocalPosition(ct.getX() + def.dx, h, ct.getZ() + def.dz);
                root()->addChild(pl);
            }
        }

        // add a spotlight on the Leonardo portrait (museum-style exhibition light)
        // Light direction is entity's -Y axis. No rotation = straight down.
        // Small X rotation tilts the beam forward for dramatic angle.
        if (leoEntity) {
            const auto leoBbox = entityBounds(leoEntity);
            const auto& leoCt = leoBbox.center();
            const float leoHeight = leoBbox.halfExtents().getY() * 2.0f;

            auto* spot = new Entity();
            spot->setEngine(engine());
            if (auto* spotComp = static_cast<LightComponent*>(spot->addComponent<LightComponent>())) {
                spotComp->setType(LightType::LIGHTTYPE_SPOT);
                spotComp->setColor(Color(1.0f, 0.95f, 0.8f));  // warm white
                spotComp->setIntensity(8.0f);
                spotComp->setRange(leoHeight * 6.0f);
                spotComp->setInnerConeAngle(10.0f);
                spotComp->setOuterConeAngle(20.0f);
                spotComp->setCastShadows(true);
                spotComp->setShadowBias(0.3f);
                spotComp->setShadowNormalBias(0.05f);
                spotComp->setShadowResolution(2048);
            }
            // Position above-front-right of the bust, aimed at its center.
            // Light direction = entity -Y axis.
            const float spotOffset = leoHeight * 1.0f;
            const float spotX = leoCt.getX() + spotOffset * 0.5f;
            const float spotY = leoCt.getY() + leoHeight * 1.5f;
            const float spotZ = leoCt.getZ() + spotOffset * 0.5f;
            spot->setLocalPosition(spotX, spotY, spotZ);

            // Aim -Y axis at the top of the bust (portrait/face area).
            const float targetY = leoCt.getY() + leoHeight * 0.5f;
            float dirX = leoCt.getX() - spotX;
            float dirY = targetY - spotY;
            float dirZ = leoCt.getZ() - spotZ;
            const float dirLen = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
            if (dirLen > 0.001f) {
                dirX /= dirLen; dirY /= dirLen; dirZ /= dirLen;
                // Rotate from=(0,-1,0) to dir. Quaternion(x,y,z,w) order.
                // cross(from, dir) = cross((0,-1,0), (dx,dy,dz)) = (-dz, 0, dx)
                // dot(from, dir) = -dy
                const float dot = -dirY;
                const float axX = -dirZ;
                const float axZ = dirX;
                const float s = std::sqrt((1.0f + dot) * 2.0f);
                const float invS = 1.0f / s;
                spot->setLocalRotation(Quaternion(axX * invS, 0.0f, axZ * invS, s * 0.5f));
            }
            root()->addChild(spot);

            spdlog::info("Spotlight: pos=({:.0f},{:.0f},{:.0f}) -> target=({:.0f},{:.0f},{:.0f}) dir=({:.2f},{:.2f},{:.2f})",
                spotX, spotY, spotZ, leoCt.getX(), leoCt.getY(), leoCt.getZ(), dirX, dirY, dirZ);
        }

        // add a ground plane
        _planeMaterial = std::make_shared<StandardMaterial>();
        _planeMaterial->setDiffuse(Color(0.2f, 0.2f, 0.2f));
        createPrimitive("plane", _planeMaterial.get(), Vector3(0.0f, -40.0f, 0.0f),
            Vector3(400.0f, 1.0f, 400.0f));

        // add shadow casting directional light
        auto* light = createDirectionalLight(Vector3(35.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(4096);
            lightComp->setShadowDistance(600.0f);
            lightComp->setShadowBias(0.4f);
            lightComp->setShadowNormalBias(0.06f);
        }

        // create camera entity — nearClip=1, farClip=600
        auto* camera = createCamera(Vector3(-60.0f, 30.0f, 60.0f));
        _cameraComp = camera->findComponent<CameraComponent>();

        if (_cameraComp && _cameraComp->camera()) {
            _cameraComp->camera()->setNearClip(1.0f);
            _cameraComp->camera()->setFarClip(600.0f);
        }

        // enable SSAO
        if (_cameraComp) {
            auto ssao = _cameraComp->ssao();
            ssao.enabled = true;
            ssao.blurEnabled = true;
            ssao.radius = 30.0f;
            ssao.samples = 12;
            ssao.intensity = 0.4f;
            ssao.power = 6.0f;
            ssao.minAngle = 10.0f;
            ssao.scale = 1.0f;
            ssao.randomize = false;
            _cameraComp->setSsao(ssao);

            // tone mapping + color finishing (fringing, grading, enhance, 3D LUT)
            auto rendering = _cameraComp->rendering();
            rendering.toneMapping = TONEMAP_NEUTRAL;
            // Values retuned 2026-08-22. They were originally picked while the
            // camera-frame rebuild silently dropped fringing/grading/enhance/LUT
            // after frame 1 (fixed in ff60028), so nothing below was ever actually
            // on screen when it was chosen — 40 fringing over a 0.8 LUT turned the
            // scene into magenta halos on a teal-orange cast.
            //
            // Fringing stays OFF here, and that is not just a taste call: compose
            // runs CAS -> SSAO -> DOF -> fringing, and applyFringing REPLACES the red
            // and blue channels with fresh samples of the raw scene texture. That
            // discards the SSAO multiply for two of the three channels, so every
            // occluded pixel keeps a darkened green over full-strength red and blue —
            // magenta tracing the exact AO footprint, at any offset size. The
            // re-sample is upstream's own design (it is why fringing must precede
            // bloom), so an SSAO showcase is simply the wrong place to combine them.
            // See post-processing-example for fringing shown on a scene without
            // compose-mode SSAO.
            rendering.fringingIntensity = 0.0f;
            rendering.gradingEnabled = true;
            rendering.gradingBrightness = 1.05f;
            rendering.gradingContrast = 1.1f;
            rendering.gradingSaturation = 1.15f;
            rendering.gradingTint[0] = 1.02f;            // subtle warm tint
            rendering.gradingTint[1] = 1.0f;
            rendering.gradingTint[2] = 0.96f;
            rendering.colorEnhanceVibrance = 0.15f;
            rendering.colorEnhanceShadows = 0.15f;       // lift shadows slightly
            if (const auto lutResource = _colorLutAsset->resource();
                lutResource && std::holds_alternative<Texture*>(*lutResource)) {
                rendering.colorLUT = std::get<Texture*>(*lutResource);
                rendering.colorLUTIntensity = 0.35f;     // teal-orange look, kept gentle
            }
            _cameraComp->setRendering(rendering);
        }

        // Setup orbit camera controls — focus on combined scene bounds
        auto sceneBbox = entityBounds(labEntity);
        if (leoEntity) {
            sceneBbox.add(entityBounds(leoEntity));
        }
        _focusPoint = sceneBbox.center();
        const float sceneRadius = std::max(sceneBbox.halfExtents().length(), 1.0f);
        _orbitDistance = std::max(sceneRadius * 2.0f, 200.0f);

        _controls = addOrbitControls(camera, _focusPoint);
        _controls->setMoveSpeed(2 * sceneRadius);
        _controls->setMoveFastSpeed(4 * sceneRadius);
        _controls->setMoveSlowSpeed(sceneRadius);
        _controls->setOrbitDistance(_orbitDistance);
        _controls->setAutoFarClip(true, 10.0f, 1000.0f);
        _controls->storeResetState();

        spdlog::info("Orbit controls: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset");
        spdlog::info("SSAO controls: O toggle SSAO, B toggle blur, Z toggle randomize");
        spdlog::info("  +/- adjust intensity, [/] adjust radius, ,/. adjust samples, ;/' adjust power");
        logSsaoState("init");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }

        auto ssao = _cameraComp->ssao();
        switch (event.key.key) {
        case SDLK_O:
            ssao.enabled = !ssao.enabled;
            _cameraComp->setSsao(ssao);
            logSsaoState("toggle");
            return true;
        case SDLK_B:
            ssao.blurEnabled = !ssao.blurEnabled;
            _cameraComp->setSsao(ssao);
            logSsaoState("blur");
            return true;
        case SDLK_Z:
            ssao.randomize = !ssao.randomize;
            _cameraComp->setSsao(ssao);
            logSsaoState("randomize");
            return true;
        case SDLK_EQUALS:
            ssao.intensity = std::min(1.0f, ssao.intensity + 0.05f);
            _cameraComp->setSsao(ssao);
            logSsaoState("intensity+");
            return true;
        case SDLK_MINUS:
            ssao.intensity = std::max(0.0f, ssao.intensity - 0.05f);
            _cameraComp->setSsao(ssao);
            logSsaoState("intensity-");
            return true;
        case SDLK_RIGHTBRACKET:
            ssao.radius = std::min(100.0f, ssao.radius + 5.0f);
            _cameraComp->setSsao(ssao);
            logSsaoState("radius+");
            return true;
        case SDLK_LEFTBRACKET:
            ssao.radius = std::max(1.0f, ssao.radius - 5.0f);
            _cameraComp->setSsao(ssao);
            logSsaoState("radius-");
            return true;
        case SDLK_PERIOD:
            ssao.samples = std::min(32, ssao.samples + 2);
            _cameraComp->setSsao(ssao);
            logSsaoState("samples+");
            return true;
        case SDLK_COMMA:
            ssao.samples = std::max(2, ssao.samples - 2);
            _cameraComp->setSsao(ssao);
            logSsaoState("samples-");
            return true;
        case SDLK_APOSTROPHE:
            ssao.power = std::min(16.0f, ssao.power + 1.0f);
            _cameraComp->setSsao(ssao);
            logSsaoState("power+");
            return true;
        case SDLK_SEMICOLON:
            ssao.power = std::max(0.5f, ssao.power - 1.0f);
            _cameraComp->setSsao(ssao);
            logSsaoState("power-");
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
        spdlog::info("*** Ambient Occlusion Example Finished ***");
    }

private:
    void logSsaoState(const char* reason) const
    {
        if (!_cameraComp) {
            return;
        }
        const auto& ssao = _cameraComp->ssao();
        spdlog::info("SSAO {}: enabled={}, blur={}, intensity={:.2f}, power={:.1f}, radius={:.1f}, samples={}, minAngle={:.1f}, scale={:.2f}, randomize={}",
            reason,
            ssao.enabled ? "ON" : "OFF",
            ssao.blurEnabled ? "ON" : "OFF",
            ssao.intensity,
            ssao.power,
            ssao.radius,
            ssao.samples,
            ssao.minAngle,
            ssao.scale,
            ssao.randomize ? "ON" : "OFF");
    }

    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _laboratory;
    std::unique_ptr<Asset> _leonardoBust;
    std::unique_ptr<Asset> _colorLutAsset;
    std::shared_ptr<StandardMaterial> _planeMaterial;

    CameraComponent* _cameraComp = nullptr;
    CameraControls* _controls = nullptr;
    Vector3 _focusPoint;
    float _orbitDistance = 200.0f;
};

VISUTWIN_EXAMPLE_MAIN(AmbientOcclusionDavinciExample)
