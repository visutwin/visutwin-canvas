// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Picker::getWorldPoint casts its ray through the CENTRE of the pixel it reads, as
// upstream's getWorldPointAsync does since 5cc6269d5. Through the integer coordinate
// it passed the pixel's top-left corner, so every picked point sat half a pick-buffer
// pixel off the surface point that pixel shows.
//
// The check needs no access to the picker's internals: a camera looking straight at a
// centred box over an EVEN buffer puts pixels 49 and 50 either side of the screen
// centre, so their points must mirror each other — which only holds when both rays go
// through pixel centres. Corner rays put pixel 50 exactly on the axis and pixel 49 a
// whole pixel to the left.
//
// It also caught the ray running BACKWARDS: built from the far plane toward the camera
// (NDC z 1 taken for the near plane under a GL-style projection), so the nearest hit
// was the far side of the object.

#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/graphics/picker.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/camera.h"
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

    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
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
        std::pair<int, int> size() const override { return {100, 100}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    Entity* addEntity(Engine& engine)
    {
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(&engine);
        engine.root()->addChild(std::move(owned));
        return entity;
    }
}

int main()
{
    auto engine = std::make_shared<Engine>(nullptr);
    AppOptions options;
    options.graphicsDevice = std::make_shared<StubDevice>();
    options.registerComponentSystem<RenderComponentSystem>();
    options.registerComponentSystem<CameraComponentSystem>();
    engine->init(options);

    auto material = std::make_shared<StandardMaterial>();
    Entity* box = addEntity(*engine);
    box->setLocalScale(4.0f, 4.0f, 1.0f);
    auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
    render->setMaterial(material.get());
    render->setType("box");

    Entity* cameraEntity = addEntity(*engine);
    cameraEntity->setLocalPosition(0.0f, 0.0f, 10.0f);   // looking down -Z at the box
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    camera->camera()->setAspectRatio(1.0f);

    Picker picker(engine.get(), 100, 100, /*depth*/ true);
    picker.prepare(camera, engine->scene().get());

    std::cout << "picked points go through pixel centres\n";
    const auto left = picker.getWorldPoint(49, 50);
    const auto right = picker.getWorldPoint(50, 50);
    const auto above = picker.getWorldPoint(50, 49);
    check(left.has_value() && right.has_value() && above.has_value(), "the box is picked at the screen centre");
    if (left && right && above) {
        std::cout << "    x at pixel 49: " << left->getX() << ", at pixel 50: " << right->getX()
                  << ", z " << right->getZ() << '\n';
        check(right->getX() > 1e-4f, "pixel 50's point is right of the axis, not on it (its centre is at 50.5)");
        check(std::fabs(left->getX() + right->getX()) < 1e-4f,
            "pixels 49 and 50 mirror each other about the screen centre");
        check(std::fabs(above->getY() + right->getY()) < 1e-4f,
            "and rows 49 and 50 do too");
        // The picker intersects bounding SPHERES (a recorded deviation), here the one
        // around the box's world AABB: centre 0, radius |(2, 2, 0.5)|. The hit must be
        // on its CAMERA side; the ray used to run from the far plane back toward the
        // camera and hit the far side (z = -2.87).
        const float radius = std::sqrt(2.0f * 2.0f + 2.0f * 2.0f + 0.5f * 0.5f);
        const float x = right->getX();
        const float y = right->getY();
        check(std::fabs(right->getZ() - std::sqrt(radius * radius - x * x - y * y)) < 1e-3f,
            "the point is on the camera side of the picked bounds");
    }

    std::cout << (failures == 0 ? "\nAll picker tests passed\n" : "\nPicker tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
