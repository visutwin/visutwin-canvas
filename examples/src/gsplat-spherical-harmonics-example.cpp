// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream gaussian-splatting/spherical-harmonics.
//
// A captured skull splat whose colour changes with the view direction: the compressed
// PLY carries spherical-harmonics bands beyond the base colour, which the splat shader
// evaluates per splat along the ray from the camera. The skull sits at (-1.5, 0.05, 0),
// turned (180, 90, 0) and scaled 0.7, on a 10 x 1 x 10 box ground (diffuse
// (0.5, 0.5, 0.4), gloss 0.2, metalness 0.5, receive-only) whose top is at y = 0.05. A
// white directional light at (55, 90, 20) casts a 2048 PCSS shadow (intensity 0.5, bias
// 0.2, normal bias 0.05, distance 10, penumbra 0.05 with falloff 4, 16 samples and 16
// blocker samples). The camera clears to grey 0.2, tone-maps ACES and orbits a pivot
// 0.2 above the skull from a distance of 4 at yaw 32 and pitch -10, out to 60. The back
// buffer is not multisampled. Turn the camera around the skull to see the SH colour
// shift.
//
// No keys beyond the orbit camera's (drag to orbit, wheel to zoom, R to reset).
//
// DEVIATIONS:
// - upstream's control panel picks the splat renderer (auto, raster with CPU sort,
//   raster with GPU sort) of its unified splat path. This engine has the classic path
//   only (one instanced draw per splat set with a background CPU depth sort), which is
//   upstream's CPU-sort raster renderer in effect, so there is nothing to choose and no
//   key for it.
// - upstream asks the splat to cast shadows and sets a scene-wide splat alpha clip of
//   0.1, which applies only to the splat's shadow and pick passes. The splat here casts
//   no shadow, so the ground shows the light alone; the colour pass is unchanged.
// - upstream's orbitCamera script (inertia 0.2, distanceMax 60) is the examples'
//   CameraControls in orbit mode around the same pivot from the same pose; there is no
//   inertia.
// - upstream caps the pixel ratio at 2; the examples render at one pixel per point.
//
#include <cmath>
#include <memory>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/gsplat/gsplatComponentSystem.h"
#include "scene/constants.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // The skull's placement, exactly as upstream authors it.
    constexpr float kSkullX = -1.5f;
    constexpr float kSkullY = 0.05f;
    constexpr float kSkullZ = 0.0f;
    constexpr float kSkullScale = 0.7f;

    // The orbit pivot is the skull's position raised by 0.2.
    constexpr float kPivotLift = 0.2f;
    constexpr float kOrbitDistance = 4.0f;
    constexpr float kOrbitYaw = 32.0f;
    constexpr float kOrbitPitch = -10.0f;
    constexpr float kOrbitDistanceMax = 60.0f;
}

class GsplatSphericalHarmonicsExample final: public ExampleApp
{
public:
    GsplatSphericalHarmonicsExample(): ExampleApp({.title = "Gaussian Splatting: Spherical Harmonics",
        .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<GSplatComponentSystem>();
    }

    bool create() override
    {
        spdlog::info("*** Gaussian Splatting Spherical Harmonics Example ***");

        // -----------------------------------------------------------------------
        // The skull splat, with its SH bands
        // -----------------------------------------------------------------------
        const std::string splatPath = assetPath("models/skull.compressed.ply");
        spdlog::info("Loading splats from '{}'...", splatPath);
        auto splatResource = GSplatResource::loadPly(splatPath, device());
        if (!splatResource) {
            spdlog::error("Splat PLY load failed");
            return false;
        }
        if (splatResource->data().shBands() == 0) {
            spdlog::warn("'{}' carries no SH bands: the splat colour will not change with the view",
                splatPath);
        }

        auto* skull = new Entity();
        skull->setName("skull");
        skull->setEngine(engine());
        root()->addChild(skull);

        auto* gsplatComponent = static_cast<GSplatComponent*>(skull->addComponent<GSplatComponent>());
        gsplatComponent->setResource(splatResource);

        skull->setLocalPosition(kSkullX, kSkullY, kSkullZ);
        skull->setLocalEulerAngles(180.0f, 90.0f, 0.0f);
        skull->setLocalScale(kSkullScale, kSkullScale, kSkullScale);

        _pivot = Vector3(kSkullX, kSkullY + kPivotLift, kSkullZ);

        // -----------------------------------------------------------------------
        // Camera with orbit controls
        // -----------------------------------------------------------------------
        // The camera starts at the orbit pose (yaw 32, pitch -10, distance 4 from the
        // pivot); CameraControls takes its orbit distance and angles from that position.
        const float yawRad = kOrbitYaw * DEG_TO_RAD;
        const float pitchRad = kOrbitPitch * DEG_TO_RAD;
        auto* cameraEntity = createCamera(Vector3(
            _pivot.getX() + kOrbitDistance * std::cos(pitchRad) * std::sin(yawRad),
            _pivot.getY() - kOrbitDistance * std::sin(pitchRad),
            _pivot.getZ() + kOrbitDistance * std::cos(pitchRad) * std::cos(yawRad)));

        if (auto* cameraComp = cameraEntity->findComponent<CameraComponent>()) {
            if (cameraComp->camera()) {
                cameraComp->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
            }
            cameraComp->setToneMapping(TONEMAP_ACES);
        }

        if (auto* controls = addOrbitControls(cameraEntity, _pivot)) {
            controls->setZoomRange(Vector2(0.0f, kOrbitDistanceMax));
            controls->storeResetState();
        }

        // -----------------------------------------------------------------------
        // Ground to receive the shadow
        // -----------------------------------------------------------------------
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.4f));
        _groundMaterial->setGloss(0.2f);
        _groundMaterial->setMetalness(0.5f);
        _groundMaterial->setUseMetalness(true);

        auto* ground = createPrimitive("box", _groundMaterial.get(),
            Vector3(0.0f, -0.45f, 0.0f), Vector3(10.0f, 1.0f, 10.0f));
        if (auto* groundRender = ground->findComponent<RenderComponent>()) {
            groundRender->setCastShadows(false);
        }

        // -----------------------------------------------------------------------
        // Shadow-casting directional light (splats are unlit; it lights the ground)
        // -----------------------------------------------------------------------
        auto* light = createDirectionalLight(Vector3(55.0f, 90.0f, 20.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
            lightComp->setShadowDistance(10.0f);
            lightComp->setShadowIntensity(0.5f);
            lightComp->setShadowResolution(2048);
            lightComp->setShadowType(ShadowType::SHADOW_PCSS_32F);
            lightComp->setPenumbraSize(0.05f);
            lightComp->setPenumbraFalloff(4.0f);
            lightComp->setShadowSamples(16);
            lightComp->setShadowBlockerSamples(16);
        }

        spdlog::info("Controls: drag to orbit, wheel to zoom, R reset, Esc quit");
        return true;
    }

    void destroy() override
    {
        spdlog::info("*** Gaussian Splatting Spherical Harmonics Example Finished ***");
    }

private:
    std::shared_ptr<StandardMaterial> _groundMaterial;
    Vector3 _pivot;
};

VISUTWIN_EXAMPLE_MAIN(GsplatSphericalHarmonicsExample)
