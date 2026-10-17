// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
// DynamicSkyEffect through the real engine on a stub device: the sun's direction from the azimuth
// and elevation, the scattering terms and the sky's balance against the sun, the light it drives
// (pointing at the sun by day and the moon by night, its colour and intensity, dimmed by the
// clouds), the night and the clouds' values (the quality interpolated between its three points),
// the sky's variant and textures for each cloud technique, its passes (the lighting before the
// scene, the cloud buffer for a volumetric sky), its mesh on the Skybox layer drawn by its camera
// only while the camera hides the scene's sky, and when the lighting is generated again (the sun
// moving past the threshold, a setting changing, bakeLighting) and when not.
//
// A sky a few degrees off, a light pointing the wrong way or a lighting regenerated every frame
// all render a plausible sky; only the numbers say they are wrong.

#include <cmath>
#include <iostream>
#include <memory>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/dynamicSkyEffect.h"
#include "scene/graphics/renderPassCameraFrame.h"
#include "scene/layer.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    Entity* addEntity(Engine& engine)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        engine.root()->addChild(std::move(owned));
        return entity;
    }

    // The lighting renders into half-float targets.
    class FloatTargetDevice final : public StubGraphicsDevice
    {
    public:
        explicit FloatTargetDevice(const Options& options) : StubGraphicsDevice(options)
        {
            setTextureHalfFloatRenderable(true);
            setTextureFloatRenderable(true);
        }
    };

    bool nearVector(const Vector3& a, const Vector3& b, const float epsilon)
    {
        return (a - b).length() <= epsilon;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<FloatTargetDevice>(StubGraphicsDevice::Options{
        .size = {64, 48}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<CameraComponentSystem, LightComponentSystem>(device);
    Scene& scene = *engine->scene();

    Entity* cameraEntity = addEntity(*engine);
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    Entity* sunEntity = addEntity(*engine);
    auto* sun = static_cast<LightComponent*>(sunEntity->addComponent<LightComponent>());
    sun->setType(LightType::LIGHTTYPE_DIRECTIONAL);

    auto sky = std::make_shared<DynamicSkyEffect>();
    sky->setLight(sun);
    sky->azimuth = 90.0f;
    sky->elevation = 30.0f;
    camera->addFrameEffect(sky);

    engine->start();
    const auto frame = [&] {
        engine->update(1.0f / 60.0f);
        engine->render();
    };
    frame();
    frame();

    std::cout << "the sun\n";
    {
        const float el = 30.0f * DEG_TO_RAD;
        check(nearVector(sky->sunDirection(), Vector3(std::cos(el), std::sin(el), 0.0f), 1e-5f),
            "azimuth 90 at elevation 30: toward +X, 30 degrees up");
        const auto& b = sky->block();
        check(near(b.sunDir[0], -std::cos(el), 1e-5f) && near(b.sunDir[1], std::sin(el), 1e-5f),
            "in sky space X is flipped");
        const double sunE = 1000.0 * std::max(0.0, 1.0 - std::exp(-((1.6110731556870734 - std::acos(std::sin(el))) / 1.5)));
        check(near(b.sunDir[3], static_cast<float>(sunE), 1e-2f), "the sun's intensity from its zenith angle");
        check(near(b.betaM[3], 1.0f * 2.0f * 0.05f / std::sin(el), 1e-5f),
            "the sky balanced against the sun: luminance x sunIntensity x 0.05 / sin(elevation)");
        check(near(b.betaR[3], 0.8f, 1e-6f), "the Mie anisotropy");
    }

    std::cout << "the light\n";
    {
        const Vector4 up = sunEntity->worldTransform().getColumn(1);
        check(nearVector(Vector3(up.getX(), up.getY(), up.getZ()), sky->sunDirection(), 1e-4f),
            "its +Y (its -Y shines) points at the sun");
        check(near(sun->intensity(), 2.0f, 1e-5f), "by day the sun's intensity, undimmed without clouds");
        check(near(sun->color().r, 1.0f, 1e-5f) && sun->color().b > 0.9f, "white above 23 degrees");
        sky->clouds = true;
        sky->cloudCoverage = 0.5f;
        frame();
        check(near(sun->intensity(), 2.0f * (1.0f - 0.5f * 0.6f), 1e-5f), "dimmed by the clouds: coverage x sunDimming");
        sky->clouds = false;
        sky->elevation = -10.0f;
        frame();
        check(near(sun->intensity(), sky->moonIntensity, 1e-5f), "at night the moon's");
        const Vector4 moonUp = sunEntity->worldTransform().getColumn(1);
        const Vector3 moon = sky->moonDirection.normalized();
        check(nearVector(Vector3(moonUp.getX(), moonUp.getY(), moonUp.getZ()), moon, 1e-4f), "pointing at the moon");
        check(near(sun->color().r, sky->moonColor.r, 1e-5f), "in the moon's colour");
        check(near(sky->block().night[3], 1.0f, 1e-6f), "the night sky fully in");
        sky->elevation = 30.0f;
        frame();
    }

    std::cout << "the clouds\n";
    {
        sky->clouds = true;
        sky->cloudQuality = 0.25f;
        frame();
        const auto& b = sky->block();
        check(near(b.cloudVolume[2], 36.0f, 1e-6f) && near(b.cloudVolume[3], 2.0f, 1e-6f) &&
              near(b.cloudShading[2], 0.425f, 1e-6f), "quality 0.25: halfway between the values at 0 and 0.5");
        check(near(b.cloudParams[2], 1000.0f / 8000.0f, 1e-7f) && near(b.cloudVolume[1], 4.5f, 1e-6f),
            "the pattern's repeats per km, the layer's top in km");
    }

    std::cout << "the sky and its passes\n";
    {
        // A perspective camera renders the volumetric clouds to a buffer.
        frame();
        check(sky->cloudsBuffered(), "volumetric clouds for a perspective camera are buffered");
        check(sky->skyVariant() == "sky-buffer", "the sky reads them from the buffer");
        const auto& preScene = camera->cameraFrame()->effectPasses().preScene;
        check(preScene.size() == 2 && preScene[0]->name() == "DynamicSkyLighting" &&
              preScene[1]->name() == "RenderPassDynamicSkyClouds", "the lighting, then the cloud buffer, before the scene");
        sky->cloudTechnique = CloudTechnique::Flat;
        frame();
        frame();
        check(sky->skyVariant() == "sky-flat" && camera->cameraFrame()->effectPasses().preScene.size() == 1,
            "flat clouds: drawn in the sky, no buffer");
        sky->clouds = false;
        frame();
        check(sky->skyVariant() == "sky", "no clouds");

        MeshInstance* skyMesh = sky->skyMeshInstance();
        check(skyMesh && skyMesh->onlyCamera() == camera->camera(), "the sky mesh is drawn by its camera only");
        const auto skybox = scene.layers()->getLayerById(LAYERID_SKYBOX);
        bool onLayer = false;
        for (const auto* instance : skybox->meshInstances()) {
            onLayer = onLayer || instance == skyMesh;
        }
        check(onLayer, "on the Skybox layer");
        check(!camera->camera()->skyVisible(), "while its camera hides the scene's sky");
    }

    std::cout << "the lighting\n";
    {
        check(sky->bakeCount() >= 1 && scene.envAtlas() == sky->envAtlas() && sky->envAtlas(),
            "generated, and the scene's environment atlas");
        const int baked = sky->bakeCount();
        frame();
        frame();
        check(sky->bakeCount() == baked, "not again while nothing changes");
        sky->elevation += 0.2f;
        frame();
        check(sky->bakeCount() == baked, "nor for a sun moved by less than the threshold");
        sky->elevation += 0.5f;
        frame();
        check(sky->bakeCount() == baked + 1, "again once it moved past it");
        sky->turbidity *= 1.005f;
        frame();
        check(sky->bakeCount() == baked + 1, "a setting changed by less than 1% does not");
        sky->turbidity *= 1.05f;
        frame();
        check(sky->bakeCount() == baked + 2, "one changed by more does");
        sky->bakeLighting();
        frame();
        check(sky->bakeCount() == baked + 3, "and bakeLighting asks for it");
    }

    std::cout << "interpolated\n";
    {
        sky->lightingInterpolation = true;
        frame();
        const int baked = sky->bakeCount();
        check(sky->blendWeight() == 1.0f, "switching it on applies the next lighting at once");
        frame();
        sky->elevation += 0.6f;
        frame();
        check(sky->bakeCount() == baked + 1 && sky->blendWeight() < 1.0f, "a gradual change blends in");
        for (int i = 0; i < 60; ++i) {
            frame();
        }
        check(sky->blendWeight() == 1.0f, "and arrives");
        sky->elevation += 10.0f;
        frame();
        check(sky->blendWeight() == 1.0f, "a jump applies at once");
    }

    std::cout << "removed\n";
    {
        check(sky->skyMeshInstance()->replacesSky(), "the sky mesh replaces the scene's");
        camera->removeFrameEffect(sky);
        frame();
        check(camera->camera()->skyVisible(),
            "the camera draws the scene's sky again, which culls the effect's (MeshInstance::replacesSky)");
    }

    return finish("dynamic sky effect");
}
