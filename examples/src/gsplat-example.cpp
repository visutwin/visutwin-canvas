// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Gaussian splatting example — port of upstream's gaussian-splatting/simple:
// a captured splat on a shadow-receiving ground plane under a PCSS directional
// light, viewed with an orbit camera. Rendered via the classic gsplat path:
// instanced screen-space EWA quads with a background CPU depth sorter for
// back-to-front blending.
//
// DEVIATION: upstream uses its own `biker` capture, whose licence PlayCanvas does
// not document. This uses a CC-BY-4.0 capture instead (see tamiya-dt03.txt), so
// the splat's own transform is fitted to that model rather than copied from
// upstream; the surrounding scene matches upstream value for value.
//
#include <cmath>
#include <memory>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/gsplat/gsplatComponentSystem.h"
#include "scene/constants.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

// Upstream's scene layout, kept as-is (the ground is 10x10 centred on the origin,
// so its top surface is at y = -0.45 + 0.5 = 0.05 and the subject stands on it).
constexpr float GROUND_TOP = 0.05f;
constexpr float SUBJECT_X = -1.5f;

// Fitted to this capture: a 180-degree flip about X puts it the right way up (raw
// 3DGS captures are Y-down, which is why upstream flips its biker too), and the
// scale brings the ~17.6-unit capture down to a ~2.6-unit subject so the orbit
// framing carries over. Offsets centre it and rest it on the ground.
constexpr float SPLAT_SCALE = 0.148f;

constexpr float ORBIT_DISTANCE = 4.0f;   // upstream ORBIT_DISTANCE
constexpr float ORBIT_YAW = 32.0f;       // upstream ORBIT_INITIAL_YAW
constexpr float ORBIT_PITCH = -10.0f;    // upstream ORBIT_INITIAL_PITCH

class GsplatExample final: public ExampleApp
{
public:
    GsplatExample(): ExampleApp({.title = "Gaussian Splatting", .width = 1200, .height = 800}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<GSplatComponentSystem>();
    }

    bool create() override
    {
        spdlog::info("*** Gaussian Splatting Example ***");

        // Upstream sets no environment at all — the splat carries its own colour and
        // the ground is lit by the directional light alone. Tone mapping is ACES and
        // exposure stays at the default 1.
        scene()->setToneMapping(TONEMAP_ACES);

        // -----------------------------------------------------------------------
        // Load the Gaussian splat PLY
        // -----------------------------------------------------------------------
        const std::string splatPath = assetPath("models/tamiya-dt03.compressed.ply");
        spdlog::info("Loading splats from '{}'...", splatPath);
        auto splatResource = GSplatResource::loadPly(splatPath, device());
        if (!splatResource) {
            spdlog::error("Splat PLY load failed");
            return false;
        }

        auto* modelEntity = new Entity();
        modelEntity->setName("splats");
        modelEntity->setEngine(engine());
        root()->addChild(modelEntity);

        auto* gsplatComponent = static_cast<GSplatComponent*>(modelEntity->addComponent<GSplatComponent>());
        gsplatComponent->setResource(splatResource);

        // Flip upright and stand it on the ground at upstream's subject offset.
        // DEVIATION: upstream sets castShadows on the gsplat component; GSplatComponent
        // has no such option here, so the splat lights nothing and casts no shadow (the
        // ground still catches the light itself). Upstream notes gsplats are unlit there too.
        modelEntity->setLocalEulerAngles(180.0f, 0.0f, 0.0f);
        modelEntity->setLocalScale(SPLAT_SCALE, SPLAT_SCALE, SPLAT_SCALE);
        modelEntity->setLocalPosition(SUBJECT_X - 0.049f, GROUND_TOP + 0.443f, -0.031f);

        // Orbit target: upstream pivots one unit above its subject's base — scaled here
        // to this subject's height so the same framing reads the same.
        _pivot = Vector3(SUBJECT_X, GROUND_TOP + 0.5f, 0.0f);

        // -----------------------------------------------------------------------
        // Lights
        // -----------------------------------------------------------------------
        // Single shadow-casting directional light, upstream's values verbatim.
        // DEVIATION: LightComponent has no shadowIntensity / shadowSamples /
        // shadowBlockerSamples setters, so upstream's 0.5 shadow intensity and its
        // 16/16 PCSS sample counts fall back to the engine defaults.
        auto* keyLight = createDirectionalLight(Vector3(55.0f, 0.0f, 20.0f),
            Color(1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* keyLightComp = keyLight->findComponent<LightComponent>()) {
            keyLightComp->setShadowType(ShadowType::SHADOW_PCSS_32F);
            keyLightComp->setShadowResolution(2048);
            keyLightComp->setShadowDistance(10.0f);
            keyLightComp->setShadowBias(0.2f);
            keyLightComp->setShadowNormalBias(0.05f);
            keyLightComp->setPenumbraSize(0.05f);
            keyLightComp->setPenumbraFalloff(4.0f);
        }

        // Ground plane to receive the shadow — upstream's box, material and transform.
        // GOTCHA: on StandardMaterial the diffuse/metalness/gloss setters are the ones
        // updateUniforms() reads; setBaseColorFactor and friends get overwritten.
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.5f, 0.5f, 0.4f));
        _groundMaterial->setGloss(0.2f);
        _groundMaterial->setMetalness(0.5f);
        _groundMaterial->setUseMetalness(true);

        auto* ground = createPrimitive("box", _groundMaterial.get(),
            Vector3(0.0f, -0.45f, 0.0f), Vector3(10.0f, 1.0f, 10.0f));
        if (auto* groundRender = ground->findComponent<RenderComponent>()) {
            groundRender->setCastShadows(false);
            groundRender->setReceiveShadows(true);
        }

        // -----------------------------------------------------------------------
        // Camera with orbit controls
        // -----------------------------------------------------------------------
        // Place the camera at upstream's initial orbit pose, then hand that pose to
        // CameraControls: setFocusPoint derives the orbit distance and angles from the
        // camera's CURRENT position without moving it, so the exact pose survives.
        const float yawRad = ORBIT_YAW * DEG_TO_RAD;
        const float pitchRad = ORBIT_PITCH * DEG_TO_RAD;
        auto* cameraEntity = createCamera(Vector3(
            _pivot.getX() + ORBIT_DISTANCE * std::cos(pitchRad) * std::sin(yawRad),
            _pivot.getY() - ORBIT_DISTANCE * std::sin(pitchRad),
            _pivot.getZ() + ORBIT_DISTANCE * std::cos(pitchRad) * std::cos(yawRad)));

        if (auto* cameraComp = cameraEntity->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
        }

        _controls = addOrbitControls(cameraEntity, _pivot);

        spdlog::info("Controls: LMB/RMB orbit, Shift/MMB pan, Wheel zoom, F focus, R reset, Esc quit");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && _controls) {
            _controls->focus(_pivot, ORBIT_DISTANCE);
            return true;
        }
        return false;
    }

    void destroy() override
    {
        spdlog::info("*** Gaussian Splatting Example Finished ***");
    }

private:
    std::shared_ptr<StandardMaterial> _groundMaterial;
    CameraControls* _controls = nullptr;
    Vector3 _pivot;
};

VISUTWIN_EXAMPLE_MAIN(GsplatExample)
