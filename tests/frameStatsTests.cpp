// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// ApplicationStats after rendering a known scene: three boxes (two materials) under one
// camera and one shadow-casting directional light, rendered by the real engine on a
// stub device whose draw() records what it was asked to draw, as the backends' do.
//
// Until 2026-09-25 fifteen of these counters had no write site: stats.cameras,
// materials, shaders, triangles, cullTime, shadowDrawCalls and every per-phase time read
// zero whatever was drawn, and the one time that was written (sort) was truncated to
// whole milliseconds and measured from the top of the forward pass. Nothing on screen
// reads them, which is how that lasted; only a count against a known scene shows it.

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
#include "scene/materials/standardMaterial.h"

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

    class StatsDevice final : public GraphicsDevice
    {
    public:
        // What a backend's draw does for the statistics, and nothing else.
        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>&, const int numInstances, int,
            bool, bool) override
        {
            recordDraw(primitive, numInstances);
        }
        // A pass the device agrees to begin is one whose execute() runs.
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

    void report(const ApplicationStats& constStats)
    {
        auto& stats = const_cast<ApplicationStats&>(constStats);
        const auto& f = stats.frame();
        const auto& d = stats.drawCalls();
        std::cout << "        cameras " << f.cameras << ", materials " << f.materials << ", shaders " << f.shaders
                  << ", triangles " << f.triangles << ", other prims " << f.otherPrimitives
                  << ", shadow map updates " << f.shadowMapUpdates << "\n        draws: forward " << d.forward
                  << ", shadow " << d.shadow << ", misc " << d.misc << ", total " << d.total
                  << "\n        ms: forward " << f.forwardTime << ", cull " << f.cullTime << ", sort " << f.sortTime
                  << ", shadow " << f.shadowMapTime << "\n";
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StatsDevice>();
    auto engine = std::make_shared<Engine>(nullptr);
    AppOptions options;
    options.graphicsDevice = device;
    options.registerComponentSystem<RenderComponentSystem>();
    options.registerComponentSystem<LightComponentSystem>();
    options.registerComponentSystem<CameraComponentSystem>();
    engine->init(options);

    Entity* cameraEntity = addEntity(*engine, "Camera");
    cameraEntity->setLocalPosition(0.0f, 0.0f, 12.0f);
    cameraEntity->addComponent<CameraComponent>();

    Entity* lightEntity = addEntity(*engine, "Light");
    lightEntity->setLocalEulerAngles(-60.0f, 0.0f, 0.0f);
    auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
    light->setCastShadows(true);

    auto materialA = std::make_shared<StandardMaterial>();
    auto materialB = std::make_shared<StandardMaterial>();
    for (int i = 0; i < 3; ++i) {
        Entity* box = addEntity(*engine, "Box" + std::to_string(i));
        box->setLocalPosition(static_cast<float>(i - 1) * 2.5f, 0.0f, 0.0f);
        auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
        render->setMaterial(i < 2 ? materialA.get() : materialB.get());
        render->setType("box");
    }

    engine->start();
    const auto& stats = *engine->stats();

    std::cout << "one camera, three boxes, two materials, one shadow-casting directional light\n";
    for (int frame = 1; frame <= 2; ++frame) {
        engine->update(1.0f / 60.0f);
        engine->render();
        std::cout << "    frame " << frame << ":\n";
        report(stats);
        auto& s = const_cast<ApplicationStats&>(stats);
        const std::string suffix = frame == 1 ? "" : " (again: the counters reset each frame)";
        check(s.frame().cameras == 1, "cameras = 1" + suffix);
        check(s.drawCalls().forward == 3, "forward draw calls = 3" + suffix);
        check(s.frame().materials == 2, "material switches = 2: A, A, B sorted by material" + suffix);
        check(s.drawCalls().shadow == 3 && s.frame().shadowMapUpdates == 1,
            "3 caster draws into 1 shadow map face" + suffix);
        check(s.frame().triangles == 6 * 12, "triangles = 6 box draws x 12, forward and shadow" + suffix);
        check(s.drawCalls().total == s.drawCalls().forward + s.drawCalls().shadow + s.drawCalls().misc &&
              s.drawCalls().misc >= 0, "total = forward + shadow + misc" + suffix);
        check(s.frame().forwardTime > 0.0 && s.frame().cullTime > 0.0 && s.frame().shadowMapTime > 0.0,
            "the forward, cull and shadow phases each took measurable (fractional) time" + suffix);
        check(s.frame().sortTime >= 0.0 && s.frame().sortTime < s.frame().forwardTime,
            "the sort is timed on its own, inside the forward time" + suffix);
    }

    std::cout << "\na second camera\n";
    {
        Entity* second = addEntity(*engine, "Camera2");
        second->setLocalPosition(0.0f, 0.0f, 15.0f);
        auto* camera2 = static_cast<CameraComponent*>(second->addComponent<CameraComponent>());
        camera2->setPriority(1);
        engine->update(1.0f / 60.0f);
        engine->render();
        report(stats);
        auto& s = const_cast<ApplicationStats&>(stats);
        check(s.frame().cameras == 2, "cameras = 2");
        check(s.drawCalls().forward == 6, "forward draw calls = 6");
        second->setEnabled(false);
        engine->update(1.0f / 60.0f);
        engine->render();
        check(s.frame().cameras == 1 && s.drawCalls().forward == 3, "disabled again: back to 1 camera, 3 draws");
    }

    std::cout << (failures == 0 ? "\nAll frame stats tests passed\n" : "\nFrame stats tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
