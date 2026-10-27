// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Port of upstream compute/histogram.
//
// A spinning icosahedron over the helipad skybox, rendered by a camera that grabs its scene
// colour. A compute pass in the camera's after-passes builds the luminance histogram of that
// colour, 256 bins counted with atomics in a storage buffer, in the same frame the camera
// rendered it; the histogram is read back and drawn as yellow lines in front of the camera.
//
// DEVIATIONS:
// - upstream clears the storage buffer with a GPU clear in the pass's onBefore; this port
//   dispatches a clearing kernel first in the same compute pass.
// - upstream reads the buffer back asynchronously in the pass's onAfter, the histogram trailing
//   a few frames; there is no asynchronous buffer read here, so the update reads the previous
//   frame's histogram with VertexBuffer::read, which waits for that frame to finish.
// - the kernel is Slang, compiled at run time; it imports the engine's `vtscenemaps` and
//   `vtdispatch` modules where upstream's WGSL includes the sceneColorCS and dispatchThreadsCS
//   chunks, and receives the dispatch in its own uniform block.
// - upstream's rotator script is the example's update; the compute stats of its MiniStats are
//   not shown.
//
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/computePass.h"
#include "platform/graphics/shader.h"
#include "scene/shader-lib/slangShaders.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"

using namespace visutwin::canvas;

namespace
{
    constexpr uint32_t kNumBins = 256;

    // The bins at u0 / binding 0, the dispatch block after them (the table in compute.h); the
    // scene colour map binds at the fixed slots of `vtscenemaps`.
    constexpr const char* kHistogramSlang = R"slang(
import vtscenemaps;
import vtdispatch;

struct HistogramParams
{
    // The threads the dispatch was set up for (Compute::dispatchThreads)
    uint4 dispatch;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> bins : register(u0);
[[vk::binding(1, 0)]] ConstantBuffer<HistogramParams> params : register(b1);

float luminance(float3 color)
{
    return saturate(dot(color, float3(0.2126, 0.7152, 0.0722)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void histogramMain(uint3 id : SV_DispatchThreadID)
{
    // a thread for each pixel - skip the threads beyond the image, as the dispatch is rounded up
    // to whole workgroups
    if (!dispatchThreadInRange(id, params.dispatch)) {
        return;
    }

    // the buffer's length is not queryable on Metal: the example's 256 bins
    const uint binCount = 256u;
    const float numBins = float(binCount);
    const uint lastBinIndex = binCount - 1u;

    // the histogram of the image as displayed - the camera stores the colour gamma encoded already,
    // in which case the conversion does nothing
    const float3 color = sceneColorToDisplay(sceneColorLoad(int2(id.xy), 0).rgb);

    const float v = luminance(color);
    const uint bin = min(uint(v * numBins), lastBinIndex);
    uint previous;
    InterlockedAdd(bins[bin], 1u, previous);
}
)slang";

    constexpr const char* kClearSlang = R"slang(
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> bins : register(u0);

[shader("compute")]
[numthreads(64, 1, 1)]
void clearMain(uint3 id : SV_DispatchThreadID)
{
    bins[id.x] = 0u;
}
)slang";
}

class HistogramExample final : public ExampleApp
{
public:
    HistogramExample() : ExampleApp({.title = "Compute Histogram"}) {}

protected:
    bool create() override
    {
        _solidAsset = std::make_unique<Asset>("solid", AssetType::CONTAINER, assetPath("models/icosahedron.glb"));
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* container = _solidAsset->resourceAs<ContainerResource>();
        Texture* helipad = _helipadAsset->resourceAs<Texture>();
        if (!container || !helipad) {
            spdlog::error("histogram needs models/icosahedron.glb and the helipad env atlas");
            return false;
        }

        // Setup skydome
        scene()->setSkyboxMip(2);
        scene()->setSkyboxIntensity(0.3f);
        scene()->setEnvAtlas(helipad);

        // The camera, which renders the scene's colour map: the input of the histogram
        Entity* cameraEntity = createCamera(Vector3(0.0f, 0.0f, 5.0f));
        _camera = cameraEntity->findComponent<CameraComponent>();
        _camera->setToneMapping(TONEMAP_ACES);
        _camera->requestSceneColorMap(true);

        createDirectionalLight(Vector3(45.0f, 0.0f, 40.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 15.0f);

        // The spinning mesh
        _solid = container->instantiateRenderEntity();
        if (!_solid) {
            spdlog::error("histogram: icosahedron.glb has no render entity");
            return false;
        }
        _solid->setEngine(engine());
        root()->addChild(_solid);
        _solid->setLocalPosition(0.0f, 0.4f, 0.0f);
        _solid->setLocalScale(0.35f, 0.35f, 0.35f);

        if (!device()->supportsCompute()) {
            return true;
        }

        // A storage buffer the compute shader writes the histogram to, 4 bytes a bin
        VertexBufferOptions bufferOptions;
        bufferOptions.data.assign(kNumBins * sizeof(uint32_t), 0);
        _bins = device()->createVertexBuffer(std::make_shared<VertexFormat>(4, true, false),
            static_cast<int>(kNumBins), bufferOptions);

        _histogramShader = getOrCreateSlangShaderFromSource(device().get(), "histogram-compute", kHistogramSlang);
        _clearShader = getOrCreateSlangShaderFromSource(device().get(), "histogram-clear", kClearSlang);
        if (!_bins || !_histogramShader || !_clearShader) {
            spdlog::error("histogram: the compute shaders or the storage buffer could not be created");
            return false;
        }

        // Clears the buffer, to avoid the accumulation buildup
        _clear = std::make_unique<Compute>(device().get(), _clearShader, "HistogramClear");
        _clear->setParameter("bins", _bins);
        _clear->setupDispatchThreads(kNumBins);

        // The histogram reads the scene colour map of the camera, attached once: each dispatch
        // reads the colour map the camera rendered most recently
        _histogram = std::make_unique<Compute>(device().get(), _histogramShader, "ComputeHistogram");
        _histogram->setSceneColorMap(_camera->sceneColorMapHandle());
        _histogram->setParameter("bins", _bins);

        // The compute shader runs after the camera renders, so it processes the colour map of the
        // frame the camera rendered
        _computePass = std::make_shared<ComputePass>(device(), std::vector<Compute*>{_clear.get(), _histogram.get()});
        _computePass->setName("HistogramPass");
        // A thread for each texel of the colour map, sized as the pass runs: under a render
        // scale or a render target the map is not the size of the canvas. Until the camera has
        // rendered a map the pass dispatches nothing.
        _computePass->setOnBefore([this](RenderPass& pass) {
            const auto& handle = _camera->sceneColorMapHandle();
            const uint32_t width = handle ? handle->width() : 0u;
            const uint32_t height = handle ? handle->height() : 0u;
            pass.setExecuteEnabled(width > 0u && height > 0u);
            if (width == 0u || height == 0u) {
                return;
            }
            _histogram->setupDispatchThreads(width, height);
            const auto dispatch = _histogram->dispatchThreads();
            _histogram->setUniformBlock(dispatch.data(), sizeof(dispatch));
        });
        _camera->afterPasses().push_back(_computePass);
        return true;
    }

    void update(const float dt) override
    {
        _solid->rotate(5.0f * dt, 10.0f * dt, -15.0f * dt);

        // Read back the histogram of the previous frame, which this waits for
        if (_bins && _computePass) {
            std::array<uint32_t, kNumBins> data{};
            if (_bins->read(0, sizeof(data), data.data())) {
                constexpr float scale = 1.0f / 50000.0f;
                _histogramPositions.clear();
                for (uint32_t x = 0; x < kNumBins; ++x) {
                    const float value = std::clamp(static_cast<float>(data[x]) * scale, 0.0f, 0.2f);
                    _histogramPositions.emplace_back(static_cast<float>(x) * 0.001f, -0.35f, 4.0f);
                    _histogramPositions.emplace_back(static_cast<float>(x) * 0.001f, value - 0.35f, 4.0f);
                }
            }
        }

        // Render the most recently read histogram using lines
        for (size_t i = 0; i + 1 < _histogramPositions.size(); i += 2) {
            scene()->drawLine(_histogramPositions[i], _histogramPositions[i + 1], Color(1.0f, 1.0f, 0.0f, 1.0f));
        }
    }

    void destroy() override
    {
        // The pass is the camera's, and the computes and the buffer hold device resources.
        if (_camera) {
            _camera->afterPasses().clear();
        }
        _computePass.reset();
        _histogram.reset();
        _clear.reset();
        _histogramShader.reset();
        _clearShader.reset();
        _bins.reset();
    }

private:
    std::unique_ptr<Asset> _solidAsset;
    std::unique_ptr<Asset> _helipadAsset;
    CameraComponent* _camera = nullptr;
    Entity* _solid = nullptr;

    std::shared_ptr<VertexBuffer> _bins;
    std::shared_ptr<Shader> _histogramShader;
    std::shared_ptr<Shader> _clearShader;
    std::unique_ptr<Compute> _clear;
    std::unique_ptr<Compute> _histogram;
    std::shared_ptr<ComputePass> _computePass;
    std::vector<Vector3> _histogramPositions;
};

VISUTWIN_EXAMPLE_MAIN(HistogramExample)
