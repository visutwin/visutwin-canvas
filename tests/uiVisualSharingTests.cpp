// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// How UI visuals share what they draw with. None of it changes a pixel — a visual with
// buffers and a material of its own draws the same image — so the frame cannot hold it:
//   - RangeAllocator hands out runs first-fit and merges what comes back;
//   - UiGeometryArena puts every block in shared buffers at its own run, writes the
//     indices absolute, keeps a returned run out of use for maxFramesInFlight frames
//     (on Metal a frame in flight still reads it), and grows by whole chunks;
//   - elements whose materials would be equal draw with ONE material, an element
//     restyled alone keeps its material object, and one restyled back shares again;
//   - a resize gives the visual new geometry and nothing else: the same mesh
//     instance, the same material, the same buffers;
//   - the mask walk is skipped once no element is a mask, after one walk that clears
//     what the masks left;
//   - a cloned element does not carry a copy of the source's visual.

#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "core/rangeAllocator.h"
#include "framework/appOptions.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/input/elementInput.h"
#include "framework/input/uiGeometryArena.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    // Buffers that keep their bytes on the CPU, so the test can read what was written.
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
        bool setData(const std::vector<uint8_t>& data) override
        {
            _storage = data;
            return true;
        }
        bool writeRange(const size_t offset, const void* data, const size_t size) override
        {
            if (offset + size > _storage.size()) {
                return false;
            }
            std::memcpy(_storage.data() + offset, data, size);
            return true;
        }
    };

    int vertexBuffersCreated = 0;

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
            ++vertexBuffersCreated;
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
        std::pair<int, int> size() const override { return {800, 600}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
        int maxFramesInFlight() const override { return 3; }
    };

    /// `quads` quads whose first position x is `tag`, so a block's bytes can be told apart.
    void makeQuads(const int quads, const float tag, std::vector<float>& vertices, std::vector<uint32_t>& indices)
    {
        vertices.assign(static_cast<size_t>(quads) * 4u * UiGeometryArena::kFloatsPerVertex, 0.0f);
        vertices[0] = tag;
        indices.clear();
        for (int q = 0; q < quads; ++q) {
            const auto base = static_cast<uint32_t>(q * 4);
            indices.insert(indices.end(), {base, base + 2u, base + 1u, base, base + 3u, base + 2u});
        }
    }

    float firstFloatOf(const UiGeometryArena::Block& block)
    {
        float value = 0.0f;
        const size_t offset = static_cast<size_t>(block.firstVertex()) * UiGeometryArena::kFloatsPerVertex * sizeof(float);
        std::memcpy(&value, block.vertexBuffer()->storage().data() + offset, sizeof(value));
        return value;
    }

    uint32_t indexAt(const UiGeometryArena::Block& block, const uint32_t i)
    {
        uint32_t value = 0;
        std::memcpy(&value, block.indexBuffer()->storage().data() + (block.firstIndex() + i) * sizeof(uint32_t),
            sizeof(value));
        return value;
    }

    /// The element's visual: the render component on its child entity.
    RenderComponent* visualOf(Entity* elementEntity)
    {
        RenderComponent* found = nullptr;
        for (const auto& child : elementEntity->children()) {
            auto* entity = dynamic_cast<Entity*>(child.get());
            if (auto* render = entity ? entity->findComponent<RenderComponent>() : nullptr) {
                found = render;
            }
        }
        return found;
    }

    int visualCount(Entity* elementEntity)
    {
        int count = 0;
        for (const auto& child : elementEntity->children()) {
            auto* entity = dynamic_cast<Entity*>(child.get());
            count += entity && entity->findComponent<RenderComponent>() ? 1 : 0;
        }
        return count;
    }

    MeshInstance* instanceOf(Entity* elementEntity)
    {
        RenderComponent* render = visualOf(elementEntity);
        return render && !render->meshInstances().empty() ? render->meshInstances().front() : nullptr;
    }
}

int main()
{
    std::cout << "range allocator\n";
    {
        RangeAllocator ranges(16);
        const auto a = ranges.allocate(4);
        const auto b = ranges.allocate(4);
        const auto c = ranges.allocate(8);
        check(a && b && c && *a == 0 && *b == 4 && *c == 8, "runs come first-fit, one after another");
        check(!ranges.allocate(1) && ranges.freeTotal() == 0, "a full capacity hands out nothing");
        ranges.release(*b, 4);
        check(!ranges.allocate(5), "a run longer than any free one is refused");
        const auto d = ranges.allocate(3);
        check(d && *d == 4, "a freed run is used again");
        ranges.release(*a, 4);
        ranges.release(*c, 8);
        ranges.release(*d, 3);
        check(ranges.allFree() && ranges.freeRunCount() == 1, "everything returned merges back into one run");
        const auto whole = ranges.allocate(16);
        check(whole && *whole == 0, "which can be handed out whole");
        check(!RangeAllocator(8).allocate(0), "an empty request gets nothing");
    }

    std::cout << "\ngeometry arena\n";
    {
        StubDevice device;
        auto arena = UiGeometryArena::create(&device);
        std::vector<float> vertices;
        std::vector<uint32_t> indices;

        makeQuads(1, 11.0f, vertices, indices);
        auto first = arena->allocate(vertices, indices);
        makeQuads(2, 22.0f, vertices, indices);
        auto second = arena->allocate(vertices, indices);
        check(first && second, "two blocks are stored");
        check(first->vertexBuffer() && first->vertexBuffer() == second->vertexBuffer() &&
            first->indexBuffer() == second->indexBuffer(), "in the same vertex and index buffers");
        check(arena->chunkCount() == 1, "of one chunk");
        check(second->firstVertex() >= first->firstVertex() + first->vertexCount() &&
            second->firstIndex() >= first->firstIndex() + first->indexCount(), "at runs that do not overlap");
        check(firstFloatOf(*first) == 11.0f && firstFloatOf(*second) == 22.0f, "each block's vertices are at its own run");
        check(indexAt(*second, 0) == second->firstVertex() && indexAt(*second, 1) == second->firstVertex() + 2u &&
            indexAt(*second, 6) == second->firstVertex() + 4u, "indices are stored absolute, offset by the first vertex");
        check(firstFloatOf(*first) == 11.0f, "and writing one block leaves the other's bytes alone");

        // A returned run is out of use for maxFramesInFlight (3) frames.
        const uint32_t freedVertex = first->firstVertex();
        first.reset();
        makeQuads(1, 33.0f, vertices, indices);
        arena->beginFrame();
        auto third = arena->allocate(vertices, indices);
        check(third && third->firstVertex() != freedVertex, "a run just given back is not handed out the next frame");
        arena->beginFrame();
        auto fourth = arena->allocate(vertices, indices);
        check(fourth && fourth->firstVertex() != freedVertex, "nor the frame after");
        arena->beginFrame();
        auto fifth = arena->allocate(vertices, indices);
        check(fifth && fifth->firstVertex() == freedVertex, "but is once three frames have begun");

        // Growth: more than the first chunk holds.
        const int before = vertexBuffersCreated;
        makeQuads(static_cast<int>(UiGeometryArena::kFirstChunkVertices / 4u), 44.0f, vertices, indices);
        auto large = arena->allocate(vertices, indices);
        check(large && arena->chunkCount() == 2 && vertexBuffersCreated == before + 1,
            "a block that does not fit goes into a second chunk");
        check(firstFloatOf(*large) == 44.0f && indexAt(*large, 1) == large->firstVertex() + 2u,
            "written the same way");
        makeQuads(1, 55.0f, vertices, indices);
        auto small = arena->allocate(vertices, indices);
        check(small && small->vertexBuffer() == second->vertexBuffer() && vertexBuffersCreated == before + 1,
            "while a small block still goes into the first");
        large.reset();
        for (int i = 0; i < 3; ++i) {
            arena->beginFrame();
        }
        check(arena->chunkCount() == 1, "a chunk nothing lives in is dropped once its runs are free");

        // A block may outlive the arena (a mesh kept past its ElementInput).
        arena.reset();
        check(firstFloatOf(*second) == 22.0f, "a block outliving the arena keeps its buffers");
        second.reset();
        third.reset();
        fourth.reset();
        fifth.reset();
        small.reset();
        check(true, "and is released without an arena to return to");

        check(UiGeometryArena::create(&device)->allocate({}, {}) == nullptr, "no geometry, no block");
    }

    // ---- ElementInput ------------------------------------------------------------
    auto device = std::make_shared<StubDevice>();
    auto engine = std::make_shared<Engine>(nullptr);
    auto elementInput = std::make_shared<ElementInput>();
    AppOptions options;
    options.graphicsDevice = device;
    options.elementInput = elementInput;
    options.registerComponentSystem<RenderComponentSystem>();
    options.registerComponentSystem<ScreenComponentSystem>();
    options.registerComponentSystem<ElementComponentSystem>();
    engine->init(options);

    auto* screen = new Entity();
    screen->setEngine(engine.get());
    engine->root()->addChild(screen);
    static_cast<ScreenComponent*>(screen->addComponent<ScreenComponent>())->setScreenSpace(true);

    const auto addImage = [&](Entity* parent, const float width, const Color& color) {
        auto* entity = new Entity();
        entity->setEngine(engine.get());
        parent->addChild(entity);
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        element->setup({.type = ElementType::Image, .anchor = Vector4(0, 0, 0, 0), .pivot = Vector2(0, 0),
                        .width = width, .height = 20.0f});
        element->setColor(color);
        return element;
    };

    const Color red(1.0f, 0.0f, 0.0f, 1.0f);
    const Color blue(0.0f, 0.0f, 1.0f, 1.0f);

    std::cout << "\nelements with equal materials share one\n";
    ElementComponent* a = addImage(screen, 100.0f, red);
    ElementComponent* b = addImage(screen, 60.0f, red);
    ElementComponent* c = addImage(screen, 60.0f, blue);
    elementInput->syncElements();
    MeshInstance* ia = instanceOf(a->entity());
    MeshInstance* ib = instanceOf(b->entity());
    MeshInstance* ic = instanceOf(c->entity());
    check(ia && ib && ic, "each image element has a mesh instance");
    if (!ia || !ib || !ic) {
        return 1;
    }
    check(ia->material() == ib->material(), "two images of one colour draw with one material");
    check(ic->material() != ia->material(), "an image of another colour has another");
    check(ia->mesh() != ib->mesh() && ia->mesh()->getVertexBuffer() == ib->mesh()->getVertexBuffer() &&
        ia->mesh()->getIndexBuffer() == ic->mesh()->getIndexBuffer(), "all three draw from the same buffers");
    {
        const Primitive pa = ia->mesh()->getPrimitive();
        const Primitive pb = ib->mesh()->getPrimitive();
        check(pa.count == 6 && pb.count == 6 && (pa.base + pa.count <= pb.base || pb.base + pb.count <= pa.base),
            "at index ranges of their own");
    }

    std::cout << "\nrestyling\n";
    {
        Material* shared = ia->material();
        b->setColor(blue);
        elementInput->syncElements();
        check(ia->material() == shared && static_cast<StandardMaterial*>(shared)->emissive().r == 1.0f,
            "restyling one of two sharers leaves the other's material as it was");
        check(ib->material() == ic->material(), "and the restyled one joins the material of its new colour");
        b->setColor(red);
        elementInput->syncElements();
        check(ib->material() == shared, "restyled back, it shares the first again");

        // c is alone with blue: a new colour restyles its own material.
        Material* own = ic->material();
        c->setColor(Color(0.0f, 1.0f, 0.0f, 1.0f));
        elementInput->syncElements();
        check(ic->material() == own && static_cast<StandardMaterial*>(own)->emissive().g == 1.0f,
            "an element nobody shares with keeps its material object, restyled");
        c->setOpacity(0.5f);
        elementInput->syncElements();
        check(ic->material() == own && static_cast<StandardMaterial*>(own)->opacity() == 0.5f, "for opacity too");
        // ... and then a sharer can find it under its new key.
        b->setColor(Color(0.0f, 1.0f, 0.0f, 1.0f));
        b->setOpacity(0.5f);
        elementInput->syncElements();
        check(ib->material() == own, "and is found under its new style by an element that takes it up");
        b->setColor(red);
        b->setOpacity(1.0f);
        elementInput->syncElements();
    }

    std::cout << "\nresizing\n";
    {
        Mesh* mesh = ia->mesh();
        Material* material = ia->material();
        const auto buffer = mesh->getVertexBuffer();
        const int base = mesh->getPrimitive().base;
        const int created = vertexBuffersCreated;
        a->setWidth(250.0f);
        elementInput->syncElements();
        check(instanceOf(a->entity()) == ia, "a resized element keeps its mesh instance");
        check(ia->mesh() == mesh && ia->material() == material, "its mesh object and its material");
        check(mesh->getVertexBuffer() == buffer && vertexBuffersCreated == created, "and creates no buffer");
        check(mesh->getPrimitive().base != base && mesh->getPrimitive().count == 6,
            "its geometry is a new run, the old one left for the frames in flight");
        // The new run holds the new width: some vertex of the quad has x = 250.
        bool found = false;
        const auto& storage = buffer->storage();
        const auto& indexStorage = mesh->getIndexBuffer()->storage();
        for (int i = 0; i < 6; ++i) {
            uint32_t index = 0;
            std::memcpy(&index, indexStorage.data() + (static_cast<size_t>(mesh->getPrimitive().base) + i) * 4u, 4u);
            float x = 0.0f;
            std::memcpy(&x, storage.data() + static_cast<size_t>(index) * UiGeometryArena::kFloatsPerVertex * 4u, 4u);
            found = found || x == 250.0f;
        }
        check(found, "and holds the new size");
        check(mesh->aabb().halfExtents().getX() == 125.0f, "with the bounds to match");
    }

    std::cout << "\nmasks\n";
    {
        ElementComponent* mask = addImage(screen, 80.0f, red);
        ElementComponent* inside = addImage(mask->entity(), 40.0f, blue);
        mask->setMask(true);
        elementInput->syncElements();
        MeshInstance* insideInstance = instanceOf(inside->entity());
        check(inside->maskedBy() == mask && insideInstance && insideInstance->stencilFront() != nullptr,
            "an element under a mask is masked by it and tests the stencil");
        check(visualOf(mask->entity())->meshInstances().size() == 2, "the mask has its unmask draw");
        check(ia->stencilFront() == nullptr && a->maskedBy() == nullptr, "an element outside is untouched");

        mask->setMask(false);
        elementInput->syncElements();
        insideInstance = instanceOf(inside->entity());
        check(inside->maskedBy() == nullptr && insideInstance && insideInstance->stencilFront() == nullptr,
            "with the mask gone, one more walk clears what it left");
        check(visualOf(mask->entity())->meshInstances().size() == 1, "and the unmask draw goes");
        ElementComponent* late = addImage(mask->entity(), 10.0f, blue);
        elementInput->syncElements();
        check(late->maskedBy() == nullptr && instanceOf(late->entity()) &&
            instanceOf(late->entity())->stencilFront() == nullptr, "an element added while no mask exists is unmasked");
        mask->setMask(true);
        elementInput->syncElements();
        check(late->maskedBy() == mask && inside->maskedBy() == mask, "and a mask set again masks them all");
        mask->entity()->destroy();
        elementInput->syncElements();
    }

    std::cout << "\ncloning\n";
    {
        check(visualCount(a->entity()) == 1, "the source has one visual");
        Entity* copy = a->entity()->clone();
        screen->addChild(copy);
        check(visualCount(copy) == 0, "its clone does not carry a copy of it");
        elementInput->syncElements();
        check(visualCount(copy) == 1 && visualCount(a->entity()) == 1, "and gets one of its own from the next sync");
        MeshInstance* copyInstance = instanceOf(copy);
        check(copyInstance && copyInstance != ia && copyInstance->mesh() != ia->mesh() &&
            copyInstance->material() == ia->material(), "its own mesh, the shared material");
        copy->destroy();
        elementInput->syncElements();
        check(instanceOf(a->entity()) == ia, "destroying the clone leaves the source's visual alone");
    }

    std::cout << "\nan element that stops being drawable\n";
    {
        b->setType(ElementType::Group);
        elementInput->syncElements();
        check(visualCount(b->entity()) == 0, "loses its visual");
        b->setType(ElementType::Image);
        elementInput->syncElements();
        check(visualCount(b->entity()) == 1 && instanceOf(b->entity()) &&
            instanceOf(b->entity())->material() == ia->material(), "and gets a new one, shared material and all");
    }

    std::cout << (failures == 0 ? "\nAll UI visual sharing tests passed\n" : "\nUI visual sharing tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
