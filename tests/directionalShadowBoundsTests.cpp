// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// A directional light's shadow bounds (Light::setShadowBounds): one shadow map fitted to a
// world box as seen from the light, rendered once a frame and sampled by every camera, in
// place of cascades fitted to each camera.
//
// Checked through the real renderer on a stub device: that two cameras share ONE shadow (one
// fit, one pass) where without bounds each renders its own; that the map holds the bounds
// inside its border, with its depth range reaching toward the light to every caster that can
// shadow the bounds and none behind them; and that a cached shadow renders again only when the
// bounds move. A render of one camera looks right with most of this broken.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/math/matrix4.h"
#include "framework/applicationStats.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/light.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // A stub that renders half-float targets, so a VSM light keeps its type.
    class HalfFloatDevice final : public StubGraphicsDevice
    {
    public:
        explicit HalfFloatDevice(const Options& options) : StubGraphicsDevice(options)
        {
            setTextureHalfFloatRenderable(true);
        }
    };

    Entity* addEntity(Engine& engine, const std::string& name)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        entity->setName(name);
        engine.root()->addChild(std::move(owned));
        return entity;
    }

    // A point through the light's shadow matrix for cascade 0: (u, v, depth).
    Vector3 project(const Light& light, const Vector3& point)
    {
        const Matrix4 matrix = Matrix4::load(light.shadowMatrixPalette(nullptr).data());
        const Vector4 clip = matrix * Vector4(point.getX(), point.getY(), point.getZ(), 1.0f);
        return Vector3(clip.getX() / clip.getW(), clip.getY() / clip.getW(), clip.getZ() / clip.getW());
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "the light\n";
    {
        Light light(nullptr);
        light.setType(LightType::LIGHTTYPE_DIRECTIONAL);
        light.setNumCascades(2);
        light.setCascadeBlend(0.1f);
        check(light.shadowCascadeCount() == 2 && light.numShadowFaces() == 2 && light.usesCascadeBlend(),
            "without bounds: the cascades, blended");
        light.setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_NONE);
        light.setShadowBounds(BoundingBox(Vector3(0.0f), Vector3(1.0f)));
        check(light.shadowCascadeCount() == 1 && light.numShadowFaces() == 1 && !light.usesCascadeBlend() &&
                  light.numCascades() == 2,
            "with bounds: one map, no blend, the cascade count kept for later");
        check(light.shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_THISFRAME,
            "setting bounds renders a cached shadow again");
        light.setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_NONE);
        light.setShadowBounds(BoundingBox(Vector3(0.0f), Vector3(1.0f)));
        check(light.shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_NONE, "equal bounds change nothing");
        light.setShadowBounds(BoundingBox(Vector3(0.5f, 0.0f, 0.0f), Vector3(1.0f)));
        check(light.shadowUpdateMode() == ShadowUpdateType::SHADOWUPDATE_THISFRAME,
            "moved bounds render a cached shadow once more");
        light.setShadowBounds(std::nullopt);
        check(light.shadowCascadeCount() == 2 && light.usesCascadeBlend(), "cleared: the cascades again");
    }

    auto device = std::make_shared<HalfFloatDevice>(StubGraphicsDevice::Options{
        .size = {64, 64}, .cpuBuffers = true, .recordDraws = true, .renderTargets = true});
    auto engine = makeTestEngine<RenderComponentSystem, LightComponentSystem, CameraComponentSystem>(device);

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->setLocalPosition(0.0f, 2.0f, 12.0f);
    cameraEntity->addComponent<CameraComponent>();
    Entity* secondEntity = addEntity(*engine, "Camera2");
    secondEntity->setLocalPosition(8.0f, 2.0f, 8.0f);
    secondEntity->setLocalEulerAngles(0.0f, 45.0f, 0.0f);
    auto* second = static_cast<CameraComponent*>(secondEntity->addComponent<CameraComponent>());
    second->setPriority(1);

    // The light shines straight down.
    Entity* lightEntity = addEntity(*engine, "Light");
    auto* lightComponent = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    lightComponent->setType(LightType::LIGHTTYPE_DIRECTIONAL);
    lightComponent->setCastShadows(true);
    lightComponent->setNumCascades(2);
    lightComponent->setShadowResolution(512);

    // A caster in the bounds, one high above them (toward the light), one below them (behind
    // the bounds along the light) and one to the side.
    auto material = std::make_shared<StandardMaterial>();
    const auto addBox = [&](const std::string& name, const Vector3& position) {
        Entity* box = addEntity(*engine, name);
        box->setLocalPosition(position.getX(), position.getY(), position.getZ());
        auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
        render->setMaterial(material.get());
        render->setType("box");
        return render->meshInstances().front();
    };
    MeshInstance* inside = addBox("Inside", Vector3(0.0f, 1.0f, 0.0f));
    MeshInstance* above = addBox("Above", Vector3(0.0f, 30.0f, 0.0f));
    MeshInstance* below = addBox("Below", Vector3(0.0f, -20.0f, 0.0f));
    MeshInstance* beside = addBox("Beside", Vector3(30.0f, 1.0f, 0.0f));

    engine->start();
    auto& stats = const_cast<ApplicationStats&>(*engine->stats());
    const auto frame = [&] {
        engine->update(1.0f / 60.0f);
        engine->render();
    };

    std::cout << "two cameras without bounds\n";
    frame();
    frame();
    const int cascadedShadowDraws = stats.drawCalls().shadow;
    check(cascadedShadowDraws > 0, "each camera renders its own cascades");

    std::cout << "two cameras with bounds\n";
    const BoundingBox bounds(Vector3(0.0f, 1.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f));
    lightComponent->setShadowBounds(bounds);
    frame();
    frame();
    Light* light = lightComponent->light();
    check(light && light->shadowBounds().has_value() && light->shadowCascadeCount() == 1,
        "the component hands its bounds to the light");
    const LightRenderData* renderData = light ? light->getRenderData(nullptr, 0) : nullptr;
    check(renderData && renderData->visibleCastersFrame == device->renderVersion(),
        "the bounds are fitted this frame, under no camera");
    const auto has = [&](const MeshInstance* meshInstance) {
        return renderData && std::find(renderData->visibleCasters.begin(), renderData->visibleCasters.end(),
            meshInstance) != renderData->visibleCasters.end();
    };
    check(has(inside) && has(above), "the casters in the bounds and toward the light from them are drawn");
    check(!has(below) && !has(beside), "the ones behind the bounds and beside them are not");
    check(stats.drawCalls().shadow == 2 && stats.frame().shadowMapUpdates == 1,
        "ONE shadow pass for both cameras: the two casters drawn once");

    std::cout << "the fit\n";
    if (light && renderData) {
        const float border = 2.0f / 512.0f;
        bool insideBorder = true;
        bool depthInRange = true;
        for (int i = 0; i < 8; ++i) {
            const Vector3 corner = bounds.center() + bounds.halfExtents() * Vector3(
                (i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f, (i & 4) ? 1.0f : -1.0f);
            const Vector3 p = project(*light, corner);
            insideBorder = insideBorder && p.getX() >= border - 1e-4f && p.getX() <= 1.0f - border + 1e-4f &&
                p.getY() >= border - 1e-4f && p.getY() <= 1.0f - border + 1e-4f;
            depthInRange = depthInRange && p.getZ() >= 0.0f && p.getZ() <= 1.0f;
        }
        check(insideBorder, "every corner of the bounds maps inside the map's border");
        check(depthInRange, "and inside its depth range");
        const Vector3 aboveTop = project(*light, Vector3(0.0f, 30.5f, 0.0f));
        check(aboveTop.getZ() >= 0.0f && aboveTop.getZ() < 0.5f, "the range reaches toward the light to the caster above");
        const Vector3 out = project(*light, Vector3(30.0f, 1.0f, 0.0f));
        check(out.getX() > 1.0f || out.getX() < 0.0f, "a point beside the bounds maps outside the map");
        const Camera* shadowCamera = renderData->shadowCamera.get();
        check(shadowCamera && near(shadowCamera->aspectRatio(), 1.0f, 1e-6f), "square texels");
        check(shadowCamera && near(renderData->projectionCompensation,
                  shadowCamera->orthoHeight() / renderData->shadowViewport.getZ(), 1e-4f),
            "the world half size of the whole map, its border included");
        check(light->shadowCascadeDistances(nullptr)[0] >= 1e29f, "no shadow distance");
    }

    std::cout << "a cached shadow\n";
    light->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_THISFRAME);
    frame();
    check(stats.frame().shadowMapUpdates == 1, "a one-shot shadow renders once");
    frame();
    check(stats.frame().shadowMapUpdates == 0, "and not again");
    lightComponent->setShadowBounds(bounds);
    frame();
    check(stats.frame().shadowMapUpdates == 0, "equal bounds do not render it again");
    lightComponent->setShadowBounds(BoundingBox(Vector3(0.0f, 1.5f, 0.0f), Vector3(2.0f, 2.0f, 2.0f)));
    frame();
    check(stats.frame().shadowMapUpdates == 1, "moved bounds render it once more");
    frame();
    check(stats.frame().shadowMapUpdates == 0, "and only once");

    std::cout << "VSM\n";
    lightComponent->setShadowType(SHADOW_VSM_16F);
    lightComponent->setVsmBlurSize(25);
    light->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_REALTIME);
    lightComponent->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_REALTIME);
    frame();
    frame();
    renderData = light->getRenderData(nullptr, 0);
    if (light->shadowType() == SHADOW_VSM_16F) {
        check(renderData && near(renderData->shadowViewport.getX(), 13.0f / 512.0f, 1e-6f),
            "a VSM blur of 25 leaves a border of 13 texels");
    } else {
        std::cout << "  skip VSM: the stub device falls back to another shadow type\n";
    }

    std::cout << "a clone\n";
    {
        std::unique_ptr<Entity> clone(lightEntity->clone());
        auto* clonedLight = clone ? clone->findComponent<LightComponent>() : nullptr;
        check(clonedLight && clonedLight->shadowBounds().has_value() &&
                  near(clonedLight->shadowBounds()->center().getY(), 1.5f, 1e-6f),
            "copies the bounds");
    }

    std::cout << "cleared\n";
    lightComponent->setShadowType(SHADOW_PCF3_32F);
    lightComponent->setShadowBounds(std::nullopt);
    frame();
    frame();
    check(light->shadowCascadeCount() == 2 && stats.drawCalls().shadow == cascadedShadowDraws,
        "each camera renders its own cascades again");

    return finish("directional shadow bounds");
}
