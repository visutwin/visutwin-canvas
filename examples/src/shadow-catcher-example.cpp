// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.10.2026
//
// Port of upstream graphics/shadow-catcher.
//
// A marble statue, turned 140 degrees, stands under the St Peter's Square night HDRI, which
// is shown as a projected sky dome (scale 200, centre (0, 0.05, 0), writing depth) and
// lights the scene through an env atlas, at exposure 0.4. Two shadow-casting directional
// lights, a yellow one (PCSS) and a red one, by default do not light the scene at all: they
// only cast the statue's shadows onto a 50 x 50 shadow catcher, an invisible plane just
// above the dome's ground that multiplies the sky behind it by the shadow it receives. The
// camera orbits the statue through a camera frame with ACES tone mapping and bokeh depth of
// field, focused on the statue every frame; the shadow distance follows the camera distance
// so the shadows are never clipped.
//
// HDRI: "St Peter's Square Night" by Poly Haven, CC BY 4.0.
//
// Keys stand in for upstream's control panel:
//   1  Enable: the shadow catcher on/off (default on)
//   2  Affect Scene: the lights light the scene, or only cast shadows (default off)
//   3  Rotate: the two lights turn about the vertical axis (default off)
//   4  DOF: depth of field on/off (default on)
//
// DEVIATIONS:
// - the red light filters its shadow with PCSS (with the yellow light's penumbra), where
//   upstream leaves it at the default PCF3. The engine picks the directional shadow filter
//   per shader variant, so a second directional light whose shadow type differs from the
//   first one's is left unshadowed, and the catcher would show the yellow light's shadow
//   alone.
// - upstream's orbitCamera script (inertia 0.2, distanceMax 500, frameOnStart off) is the
//   examples' CameraControls in orbit mode: the pivot is the statue's bounds centre, the
//   distance is the authored camera position's, the zoom is capped at 500 and the pitch
//   at +/-90 degrees. Its damping is CameraControls' own, not the 0.2 s inertia.
// - upstream asks for an HDR display and renders untonemapped on one; there is no HDR
//   output here, so the scene takes upstream's SDR branch, ACES.
//
#include <memory>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "framework/script/shadowCatcher.h"
#include "platform/input/inputConstants.h"
#include "platform/input/keyboard.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // Early in the transparent pass, so the catcher's shadow darkens the background.
    constexpr uint8_t kCatcherDrawBucket = 250;
}

class ShadowCatcherExample final: public ExampleApp
{
public:
    ShadowCatcherExample(): ExampleApp({.title = "Shadow Catcher"}) {}

protected:
    bool create() override
    {
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _hdrAsset = std::make_unique<Asset>("hdri", AssetType::TEXTURE, assetPath("hdri/st-peters-square.hdr"));
        auto* statue = _statueAsset->resourceAs<ContainerResource>();
        auto* hdr = _hdrAsset->resourceAs<Texture>();
        if (!statue || !hdr) {
            spdlog::error("Failed to load the statue or the St Peter's Square HDRI");
            return false;
        }

        // Depth layer is where prepass finishes rendering. Move the depth layer to take place
        // after World and Skydome layers, to capture both of them in depth buffer, to be used
        // by Depth of Field
        if (const auto layers = scene()->layers()) {
            if (const auto depthLayer = layers->getLayerById(LAYERID_DEPTH)) {
                layers->remove(depthLayer);
                layers->insertOpaque(depthLayer, 2);
            }
        }

        // Add an instance of the statue
        _statue = statue->instantiateRenderEntity();
        _statue->setEngine(engine());
        for (auto* render : _statue->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
        }
        _statue->rotate(0.0f, 140.0f, 0.0f);
        root()->addChild(_statue);

        // Create an Entity with a camera component
        _camera = createCamera(Vector3(35.0f, 12.0f, -17.0f));
        _camera->lookAt(0.0f, 0.0f, 1.0f);
        _cameraComp = _camera->findComponent<CameraComponent>();
        _cameraComp->camera()->setFarClip(500.0f);
        _cameraComp->camera()->setFov(60.0f);
        _cameraComp->setToneMapping(TONEMAP_ACES);

        // Orbit the statue
        CameraControls* controls = addOrbitControls(_camera, entityBounds(_statue).center());
        controls->setPitchRange(Vector2(-90.0f, 90.0f));
        controls->setZoomRange(Vector2(0.0f, 500.0f));
        controls->storeResetState();

        // Convert the HDRI to a high resolution cubemap for the skybox, and generate an
        // env-atlas texture for the lighting
        _skybox.reset(EnvLighting::generateSkyboxCubemap(device().get(), hdr));
        _envAtlas.reset(EnvLighting::generateAtlas(device().get(), hdr));
        scene()->setSkybox(_skybox.get());
        scene()->setEnvAtlas(_envAtlas.get());

        scene()->setExposure(0.4f);
        scene()->setSkyType(SKYTYPE_DOME);
        scene()->sky()->node()->setLocalScale(Vector3(200.0f, 200.0f, 200.0f));
        scene()->sky()->node()->setLocalPosition(Vector3(0.0f, 0.0f, 0.0f));
        scene()->sky()->setCenter(Vector3(0.0f, 0.05f, 0.0f));

        // Enable depth writing for the sky, for DOF to work on it
        scene()->sky()->setDepthWrite(true);

        // Create two directional lights which cast shadows
        _light1 = createDirectionalLight(Vector3(55.0f, -90.0f, 0.0f), Color::YELLOW, 1.0f, true);
        _light1->setName("Light1");
        _light1Comp = _light1->findComponent<LightComponent>();
        _light1Comp->setShadowBias(0.1f);
        _light1Comp->setShadowNormalBias(0.3f);
        _light1Comp->setShadowDistance(50.0f);
        _light1Comp->setShadowResolution(1024);
        _light1Comp->setShadowIntensity(0.4f);
        _light1Comp->setShadowType(SHADOW_PCSS_32F);
        _light1Comp->setPenumbraSize(0.05f);
        _light1Comp->setPenumbraFalloff(4.0f);
        _light1Comp->setShadowSamples(10);
        _light1Comp->setShadowBlockerSamples(10);

        _light2 = createDirectionalLight(Vector3(45.0f, -30.0f, 0.0f), Color::RED, 1.0f, true);
        _light2->setName("Light2");
        _light2Comp = _light2->findComponent<LightComponent>();
        _light2Comp->setShadowBias(0.1f);
        _light2Comp->setShadowNormalBias(0.3f);
        _light2Comp->setShadowDistance(50.0f);
        _light2Comp->setShadowResolution(1024);
        _light2Comp->setShadowIntensity(0.5f);
        // See the DEVIATION in the header for why this is PCSS.
        _light2Comp->setShadowType(SHADOW_PCSS_32F);
        _light2Comp->setPenumbraSize(0.05f);
        _light2Comp->setPenumbraFalloff(4.0f);

        // Create an entity with a shadow catcher script, which creates a shadow catcher
        // geometry plane with a specified scale
        _shadowCatcher = new Entity();
        _shadowCatcher->setEngine(engine());
        _shadowCatcher->setName("ShadowCatcher");
        _shadowCatcher->addComponent<ScriptComponent>();
        _shadowCatcher->script()->create<ShadowCatcher>()->setPlaneScale(50.0f);

        // Offset it slightly above the ground (skydome) - this is needed when DOF is enabled
        // and the skydome writes depth to the depth buffer, to avoid depth conflicts with the
        // shadow catcher plane
        _shadowCatcher->setLocalPosition(0.0f, 0.01f, 0.0f);
        root()->addChild(_shadowCatcher);

        // Camera frame rendering, to give us access to Depth of Field
        auto rendering = _cameraComp->rendering();
        rendering.toneMapping = TONEMAP_ACES;
        _cameraComp->setRendering(rendering);

        auto dof = _cameraComp->dof();
        dof.enabled = true;
        dof.nearBlur = true;
        dof.focusDistance = 30.0f;
        dof.focusRange = 10.0f;
        dof.blurRadius = 7.0f;
        dof.blurRings = 5;
        dof.blurRingPoints = 5;
        dof.highQuality = true;
        _cameraComp->setDof(dof);

        spdlog::info("Keys: 1 = shadow catcher, 2 = affect scene, 3 = rotate lights, 4 = DOF, Esc = quit");
        return true;
    }

    void update(const float dt) override
    {
        handleKeys();

        // The catcher plane exists once its script has initialized
        if (!_catcherBucketSet) {
            for (auto* render : _shadowCatcher->findComponents<RenderComponent>()) {
                for (auto* meshInstance : render->meshInstances()) {
                    meshInstance->setDrawBucket(kCatcherDrawBucket);
                    _catcherBucketSet = true;
                }
            }
        }

        // DOF distance - distance between the camera and the entity
        const float distance = _camera->position().distance(_statue->position());
        auto dof = _cameraComp->dof();
        dof.enabled = _dof;
        dof.focusDistance = distance;
        _cameraComp->setDof(dof);

        // Adjust shadow distance to never clip them
        _light1Comp->setShadowDistance(distance + 15.0f);
        _light2Comp->setShadowDistance(distance + 15.0f);

        // Enable the shadow catcher
        _shadowCatcher->setEnabled(_catcher);

        // Rotate the light
        if (_rotate) {
            _light1->rotate(0.0f, 20.0f * dt, 0.0f);
            _light2->rotate(0.0f, -30.0f * dt, 0.0f);
        }

        // If lights should not affect the scene, they only cast shadows
        const float intensity = _affectScene ? 1.0f : 0.0f;
        _light1Comp->setIntensity(intensity);
        _light2Comp->setIntensity(intensity);
    }

    void destroy() override
    {
        // The scene borrows both; it is torn down after this
        if (scene()) {
            scene()->setSkybox(nullptr);
            scene()->setEnvAtlas(nullptr);
        }
    }

private:
    void handleKeys()
    {
        const auto* keyboard = engine()->keyboard();
        if (!keyboard) {
            return;
        }
        if (keyboard->wasPressed(Key::Digit1)) {
            _catcher = !_catcher;
            spdlog::info("Shadow catcher: {}", _catcher ? "on" : "off");
        }
        if (keyboard->wasPressed(Key::Digit2)) {
            _affectScene = !_affectScene;
            spdlog::info("Affect scene: {}", _affectScene ? "on" : "off");
        }
        if (keyboard->wasPressed(Key::Digit3)) {
            _rotate = !_rotate;
            spdlog::info("Rotate lights: {}", _rotate ? "on" : "off");
        }
        if (keyboard->wasPressed(Key::Digit4)) {
            _dof = !_dof;
            spdlog::info("DOF: {}", _dof ? "on" : "off");
        }
    }

    std::unique_ptr<Asset> _statueAsset;
    std::unique_ptr<Asset> _hdrAsset;
    std::unique_ptr<Texture> _skybox;
    std::unique_ptr<Texture> _envAtlas;

    Entity* _statue = nullptr;
    Entity* _camera = nullptr;
    CameraComponent* _cameraComp = nullptr;
    Entity* _light1 = nullptr;
    Entity* _light2 = nullptr;
    LightComponent* _light1Comp = nullptr;
    LightComponent* _light2Comp = nullptr;
    Entity* _shadowCatcher = nullptr;
    bool _catcherBucketSet = false;

    // Initial values
    bool _affectScene = false;
    bool _catcher = true;
    bool _rotate = false;
    bool _dof = true;
};

VISUTWIN_EXAMPLE_MAIN(ShadowCatcherExample)
