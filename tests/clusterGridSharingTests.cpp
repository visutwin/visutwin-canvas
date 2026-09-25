// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// One cluster grid per DISTINCT light set (Renderer::clustersForLightSet).
//
// The local-light list is per (camera, layer), so the grid has to be too. What this
// replaced built ONE grid from whichever layer rendered first and skipped the whole
// block, binding included, for every layer after: a layer with different lights was
// lit by another layer's cells, and a layer with NO clustered lights kept the previous
// layer's buffers bound and stayed lit by them. Now grids are keyed on an
// order-independent hash of the light set and pooled across frames, and every layer
// binds its own grid or zeroes the params.
//
// CPU only: a stub device records what the renderer binds.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "scene/lighting/worldClusters.h"
#include "scene/renderer/renderer.h"

namespace visutwin::canvas
{
    // The seam Renderer befriends for this test.
    struct RendererTestAccess
    {
        static WorldClusters* grid(Renderer& r, const uint64_t hash, const std::vector<ClusterLightData>& lights)
        {
            return r.clustersForLightSet(hash, lights);
        }
        static uint64_t hash(const std::vector<const void*>& members) { return Renderer::lightSetHash(members); }
        static void bind(Renderer& r, const WorldClusters* clusters) { r.bindLayerClusters(clusters); }
        static size_t poolSize(const Renderer& r) { return r._clusterPool.size(); }
    };
}

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

    class RecordingDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {64, 64}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }

        void setClusterBuffers(const void* lightData, size_t, const void*, size_t) override
        {
            ++bufferBinds;
            boundLights = lightData;
        }
        void setClusterGridParams(const float* boundsMin, const float* boundsRange, const float*,
            int cellsX, int, int, int, const int numClusteredLights) override
        {
            ++paramSets;
            lightCount = numClusteredLights;
            cells = cellsX;
            rangeX = boundsRange[0];
            minX = boundsMin[0];
        }

        int bufferBinds = 0;
        int paramSets = 0;
        const void* boundLights = nullptr;
        int lightCount = -1;
        int cells = -1;
        float rangeX = -1.0f;
        float minX = -1.0f;
    };

    ClusterLightData omni(const float x, const float range)
    {
        ClusterLightData light;
        light.position = Vector3(x, 0.0f, 0.0f);
        light.range = range;
        light.intensity = 1.0f;
        return light;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<RecordingDevice>();
    Renderer renderer(device, nullptr);

    // Three stand-in light identities: the hash only reads the addresses.
    int a = 0, b = 0, c = 0;

    std::cout << "the light-set key\n";
    {
        const auto ab = RendererTestAccess::hash({&a, &b});
        check(ab == RendererTestAccess::hash({&b, &a}), "does not depend on the order the lights were gathered in");
        check(ab != RendererTestAccess::hash({&a, &c}), "differs for a different set");
        check(ab != RendererTestAccess::hash({&a}), "and for a subset");
        check(RendererTestAccess::hash({}) != ab, "the empty set has its own key");
    }

    const std::vector<ClusterLightData> twoLights = {omni(0.0f, 5.0f), omni(10.0f, 5.0f)};
    const std::vector<ClusterLightData> oneLight = {omni(-20.0f, 2.0f)};
    const auto keyAB = RendererTestAccess::hash({&a, &b});
    const auto keyC = RendererTestAccess::hash({&c});
    const auto keyNone = RendererTestAccess::hash({});

    std::cout << "\ngrids within one frame\n";
    WorldClusters* worldGrid = nullptr;
    {
        renderer.resetClusters();
        worldGrid = RendererTestAccess::grid(renderer, keyAB, twoLights);
        // A second layer seeing the same lights. Handing it a DIFFERENT list under the
        // same key shows whether the grid was rebuilt: it must not be.
        WorldClusters* again = RendererTestAccess::grid(renderer, keyAB, oneLight);
        check(again == worldGrid, "two layers with the same light set share ONE grid");
        check(worldGrid->lightCount() == 2, "and it is built once, not rebuilt for the second layer");
        WorldClusters* other = RendererTestAccess::grid(renderer, keyC, oneLight);
        check(other != worldGrid && other->lightCount() == 1, "a layer with a different set gets its own grid");
        check(worldGrid->lightCount() == 2, "without disturbing the first");
        WorldClusters* empty = RendererTestAccess::grid(renderer, keyNone, {});
        check(empty != worldGrid && empty != other && empty->lightCount() == 0, "an empty set gets an empty grid");
        check(RendererTestAccess::poolSize(renderer) == 3, "three distinct sets, three pooled grids");
    }

    std::cout << "\nbinding, layer after layer\n";
    {
        WorldClusters* empty = RendererTestAccess::grid(renderer, keyNone, {});
        RendererTestAccess::bind(renderer, worldGrid);
        check(device->bufferBinds == 1 && device->boundLights == worldGrid->lightData(),
            "a layer with lights binds its grid's buffers");
        check(device->lightCount == 2 && device->cells == worldGrid->config().cellsX && device->rangeX > 0.0f,
            "and its grid params");
        RendererTestAccess::bind(renderer, empty);
        check(device->paramSets == 2, "the NEXT layer binds too, it does not inherit the first one's grid");
        check(device->lightCount == 0 && device->cells == 0 && device->rangeX == 0.0f && device->minX == 0.0f,
            "a layer with no clustered lights ZEROES the params, so the previous layer's lights go dark");
        check(device->bufferBinds == 1, "and binds no buffers of its own");
    }

    std::cout << "\nthe next frame\n";
    {
        renderer.resetClusters();
        WorldClusters* first = RendererTestAccess::grid(renderer, keyC, oneLight);
        check(first == worldGrid, "the pool survives the reset: the first grid asked for reuses slot 0");
        check(first->lightCount() == 1, "rebuilt for this frame's set");
        RendererTestAccess::grid(renderer, keyAB, twoLights);
        check(RendererTestAccess::poolSize(renderer) == 3, "no new allocation while the pool has grids to spare");
    }

    std::cout << (failures == 0 ? "\nAll cluster grid sharing tests passed\n" : "\nCluster grid sharing tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
