// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
// The caster lists of shadowed LOCAL lights. The scene's casters are collected and put
// through the caster rules once per frame for every local light together
// (collectLightIndependentShadowCasters), each light's faces then keep the ones inside
// them, and the shadow passes draw those lists. Three things have to hold, and none of
// them is visible in a render until a shadow is missing:
//
//  - a spot's list is exactly what its pass used to find for itself: the scene's casters
//    through shouldRenderShadowMeshInstance against the spot's shadow camera;
//  - a list is used only in the frame that prepared it (it holds raw pointers), so a light
//    whose shadow does not render gets none, and its faces draw nothing stale;
//  - the shadow draws the frame counts are the lists, no more and no less.
//
// Three boxes in a row, a spot above the middle one, an omni above the left one, rendered
// by the real engine on a stub device: under clustered lighting (one atlas pass draws
// every face) and, with --non-clustered, with a pass per face.

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "framework/appOptions.h"
#include "framework/applicationStats.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/frustumUtils.h"
#include "scene/light.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/renderer/shadowCasterFiltering.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    class CpuVertexBuffer final : public VertexBuffer
    {
    public:
        using VertexBuffer::VertexBuffer;
        void unlock() override {}
    };

    class CpuIndexBuffer final : public IndexBuffer
    {
    public:
        using IndexBuffer::IndexBuffer;
        bool setData(const std::vector<uint8_t>&) override { return true; }
    };

    class StubRenderTarget final : public RenderTarget
    {
    public:
        using RenderTarget::RenderTarget;
    protected:
        void destroyFrameBuffers() override {}
        void createFrameBuffers() override {}
    };

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>&, const int numInstances, int,
            bool, bool) override
        {
            recordDraw(primitive, numInstances);
        }
        void startRenderPass(RenderPass*) override { _insideRenderPass = true; }
        void endRenderPass(RenderPass*) override { _insideRenderPass = false; }
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options) override
        {
            return std::make_shared<CpuVertexBuffer>(this, format, numVertices, options);
        }
        std::shared_ptr<IndexBuffer> createIndexBuffer(const IndexFormat format, const int numIndices,
            const std::vector<uint8_t>& data) override
        {
            auto buffer = std::make_shared<CpuIndexBuffer>(this, format, numIndices);
            buffer->setData(data);
            return buffer;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {64, 64}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions& options) override
        {
            RenderTargetOptions withDevice = options;
            withDevice.graphicsDevice = this;
            return std::make_shared<StubRenderTarget>(withDevice);
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

    // What a spot's pass found for itself before the lists existed: every caster in the
    // scene through the caster rules and the shadow camera's frustum.
    std::vector<MeshInstance*> referenceSpotCasters(Light* light)
    {
        std::vector<MeshInstance*> result;
        LightRenderData* rd = light->getRenderData(nullptr, 0);
        if (!rd || !rd->shadowCamera || !rd->shadowCamera->node()) {
            return result;
        }
        Camera* shadowCamera = rd->shadowCamera.get();
        const Frustum frustum = buildCameraFrustum(shadowCamera, shadowCamera->node());
        std::vector<MeshInstance*> all;
        collectShadowCasters(all);
        for (auto* meshInstance : all) {
            if (meshInstance && meshInstance->visible() &&
                shouldRenderShadowMeshInstance(meshInstance, shadowCamera, frustum)) {
                result.push_back(meshInstance);
            }
        }
        return result;
    }

    bool contains(const std::vector<MeshInstance*>& list, const MeshInstance* meshInstance)
    {
        return std::find(list.begin(), list.end(), meshInstance) != list.end();
    }

    void run(const bool clustered)
    {
        std::cout << (clustered ? "\nclustered lighting (the shadow atlas pass)\n"
                                : "\nnon-clustered lighting (a pass per face)\n");

        auto device = std::make_shared<StubDevice>();
        auto engine = std::make_shared<Engine>(nullptr);
        AppOptions options;
        options.graphicsDevice = device;
        options.registerComponentSystem<RenderComponentSystem>();
        options.registerComponentSystem<LightComponentSystem>();
        options.registerComponentSystem<CameraComponentSystem>();
        engine->init(options);
        engine->scene()->setClusteredLightingEnabled(clustered);

        Entity* cameraEntity = addEntity(*engine, "Camera");
        cameraEntity->setLocalPosition(0.0f, 2.0f, 14.0f);
        cameraEntity->addComponent<CameraComponent>();

        auto material = std::make_shared<StandardMaterial>();
        std::vector<Entity*> boxes;
        std::vector<MeshInstance*> boxInstances;
        for (int i = 0; i < 3; ++i) {
            Entity* box = addEntity(*engine, "Box" + std::to_string(i));
            box->setLocalPosition(static_cast<float>(i - 1) * 2.5f, 0.0f, 0.0f);
            auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
            render->setMaterial(material.get());
            render->setType("box");
            boxes.push_back(box);
            boxInstances.push_back(render->meshInstances().front());
        }

        // A spot straight above the middle box, shining down its rest direction (-Y):
        // a 20-degree half angle is 1.1 units wide at the box, which the boxes either
        // side (2 units away at their nearest) stay clear of.
        Entity* spotEntity = addEntity(*engine, "Spot");
        spotEntity->setLocalPosition(0.0f, 3.0f, 0.0f);
        auto* spot = static_cast<LightComponent*>(spotEntity->addComponent<LightComponent>());
        spot->setType(LightType::LIGHTTYPE_SPOT);
        spot->setOuterConeAngle(20.0f);
        spot->setInnerConeAngle(10.0f);
        spot->setRange(10.0f);
        spot->setCastShadows(true);

        // An omni two units above the LEFT box with a range of three: the left box is
        // under it, the right one five units along X is out of reach.
        Entity* omniEntity = addEntity(*engine, "Omni");
        omniEntity->setLocalPosition(-2.5f, 2.0f, 0.0f);
        auto* omni = static_cast<LightComponent*>(omniEntity->addComponent<LightComponent>());
        omni->setType(LightType::LIGHTTYPE_OMNI);
        omni->setRange(3.0f);
        omni->setCastShadows(true);

        engine->start();
        auto& stats = const_cast<ApplicationStats&>(*engine->stats());
        const auto renderFrame = [&] {
            engine->update(1.0f / 60.0f);
            engine->render();
        };
        const auto listed = [&](Light* light) {
            int count = 0;
            for (int face = 0; face < light->numShadowFaces(); ++face) {
                const LightRenderData* rd = light->getRenderData(nullptr, face);
                if (rd && rd->visibleCastersFrame == device->renderVersion()) {
                    count += static_cast<int>(rd->visibleCasters.size());
                }
            }
            return count;
        };

        renderFrame();
        renderFrame();
        Light* spotLight = spot->light();
        Light* omniLight = omni->light();

        // ── The spot ─────────────────────────────────────────────────────────────
        LightRenderData* spotData = spotLight->getRenderData(nullptr, 0);
        check(spotData && spotData->visibleCastersFrame == device->renderVersion(),
            "the spot's list was prepared in the frame just rendered");
        const auto reference = referenceSpotCasters(spotLight);
        check(spotData && spotData->visibleCasters == reference,
            "and is the scene's casters through the caster rules and the cone's frustum");
        check(spotData && spotData->visibleCasters.size() == 1 && contains(spotData->visibleCasters, boxInstances[1]),
            "which is the box under it alone (" + std::to_string(spotData ? spotData->visibleCasters.size() : 0) + ")");

        // ── The omni ─────────────────────────────────────────────────────────────
        bool stamped = true;
        bool rightBoxListed = false;
        for (int face = 0; face < 6; ++face) {
            const LightRenderData* rd = omniLight->getRenderData(nullptr, face);
            stamped = stamped && rd && rd->visibleCastersFrame == device->renderVersion();
            rightBoxListed = rightBoxListed || (rd && contains(rd->visibleCasters, boxInstances[2]));
        }
        check(stamped, "all six of the omni's lists were prepared in that frame");
        // Faces look down +X, -X, +Y, -Y, +Z, -Z: face 3 is the one looking down.
        const LightRenderData* down = omniLight->getRenderData(nullptr, 3);
        check(down && contains(down->visibleCasters, boxInstances[0]), "the box under the omni is in its -Y face");
        check(!rightBoxListed, "the box out of its range is in none");

        // ── The passes draw the lists ────────────────────────────────────────────
        const int expected = listed(spotLight) + listed(omniLight);
        check(stats.drawCalls().shadow == expected && expected >= 2,
            "shadow draws = the casters listed (" + std::to_string(stats.drawCalls().shadow) + " of " +
            std::to_string(expected) + ")");

        // A caster the rules reject is in no list, whichever light looks at it.
        static_cast<RenderComponent*>(boxes[1]->findComponent<RenderComponent>())->setCastShadows(false);
        renderFrame();
        check(spotData && spotData->visibleCasters.empty() && spotData->visibleCastersFrame == device->renderVersion(),
            "a box that does not cast leaves the spot's list empty");
        check(stats.drawCalls().shadow == listed(spotLight) + listed(omniLight), "and the draws follow");
        static_cast<RenderComponent*>(boxes[1]->findComponent<RenderComponent>())->setCastShadows(true);

        // A moved caster is tested where it is now, not where a cached bound left it.
        boxes[2]->setLocalPosition(0.0f, 1.0f, 0.0f);
        renderFrame();
        check(spotData && spotData->visibleCasters.size() == 2 && contains(spotData->visibleCasters, boxInstances[2]),
            "a box moved under the spot joins its list");
        boxes[2]->setLocalPosition(2.5f, 0.0f, 0.0f);

        // ── A light whose shadow does not render gets no list ────────────────────
        spot->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_NONE);
        renderFrame();
        check(spotData && spotData->visibleCastersFrame != device->renderVersion(),
            "a spot with shadow updates off has no list for the frame");
        check(stats.drawCalls().shadow == listed(omniLight) && listed(spotLight) == 0,
            "and only the omni's casters are drawn (" + std::to_string(stats.drawCalls().shadow) + ")");
        spot->setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_REALTIME);
        renderFrame();
        check(spotData && spotData->visibleCastersFrame == device->renderVersion() &&
              spotData->visibleCasters == referenceSpotCasters(spotLight) &&
              stats.drawCalls().shadow == listed(spotLight) + listed(omniLight),
            "switched back on, it is prepared and drawn again");
    }
}

// One lighting mode per process: component instance lists are process-wide, so a second
// engine in the same process would see the first one's boxes and lights.
int main(const int argc, char** argv)
{
    std::cout << std::unitbuf;
    run(!(argc > 1 && std::string(argv[1]) == "--non-clustered"));
    std::cout << (failures == 0 ? "\nAll local shadow caster tests passed\n" : "\nLocal shadow caster tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
