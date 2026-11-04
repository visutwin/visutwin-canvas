// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Port of upstream compute/histogram.
//
// A trefoil knot floats above a floor under three coloured spot lights (pink, cyan, amber) that
// circle above it, a third of a turn apart, rising and falling and casting shadows; where their
// light overlaps the colours add to white. The camera (neutral tone mapping, a black clear colour,
// the floor fading into the black through linear fog 8 - 18) grabs its scene colour, and a compute
// pass among its after-passes builds the histogram of the picture as displayed - 256 bins each for
// red, green, blue and luminance - in the frame it rendered: each workgroup of 256 threads counts
// its pixels into bins of its own in shared memory, then adds them to the storage buffer once. A
// panel in the bottom right corner draws it: the red, green and blue areas added together (white
// where they overlap) and the luminance as a white line, on a logarithmic scale, over a dark
// rounded rectangle with a tone ramp underneath. The product studio's environment (intensity 0.15)
// gives the knot soft reflections.
//
// Keys stand in for upstream's controls panel:
//   [ / ]        the exposure -/+ 0.25 (0.25 - 3)
//   , / .        the speed -/+ 0.25 (0 - 3)
//
// DEVIATIONS:
// - The panel's shader cannot bind the storage buffer here, so the update reads the previous
//   frame's histogram back (VertexBuffer::read, which waits for that frame) into a 256 x 1 float
//   texture, which an alpha-blended quad pass appended to the frame draws as the panel; upstream's
//   ShaderMaterial reads the buffer directly. The panel has no "Histogram" and "R G B L" labels.
// - The buffer is cleared by a kernel dispatched first in the same compute pass (upstream clears
//   it in the pass's onBefore).
// - The kernels are Slang, compiled at run time; they import the engine's `vtscenemaps` and
//   `vtdispatch` modules where upstream's WGSL includes the sceneColorCS and dispatchThreadsCS
//   chunks, and receive the dispatch in their own uniform block.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/computePass.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    constexpr uint32_t kNumBins = 256;
    constexpr uint32_t kChannels = 4;   // red, green, blue, luminance

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

static const uint numBins = 256u;

// The workgroup's own bins: most pixels fall into a few bins, and threads adding to one bin wait
// for each other, so a workgroup counts its pixels here first and adds them to the histogram once.
groupshared uint localBins[4u * numBins];

// the bin a value between 0 and 1 falls into
uint binIndex(float value)
{
    return min(uint(saturate(value) * float(numBins)), numBins - 1u);
}

// a workgroup of 256 threads, one for each bin of a channel
[shader("compute")]
[numthreads(16, 16, 1)]
void histogramMain(uint3 id : SV_DispatchThreadID, uint localIndex : SV_GroupIndex)
{
    for (uint channel = 0u; channel < 4u; ++channel) {
        localBins[channel * numBins + localIndex] = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // a thread for each pixel - those beyond the image (the dispatch is rounded up to whole
    // workgroups) count none, but still add the bins to the histogram
    if (dispatchThreadInRange(id, params.dispatch)) {
        // the colour as displayed - stored gamma encoded already, the conversion does nothing
        const float3 color = sceneColorToDisplay(sceneColorLoad(int2(id.xy), 0).rgb);
        const float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
        uint previous;
        InterlockedAdd(localBins[binIndex(color.r)], 1u, previous);
        InterlockedAdd(localBins[numBins + binIndex(color.g)], 1u, previous);
        InterlockedAdd(localBins[2u * numBins + binIndex(color.b)], 1u, previous);
        InterlockedAdd(localBins[3u * numBins + binIndex(luminance)], 1u, previous);
    }
    GroupMemoryBarrierWithGroupSync();

    // each thread adds one bin of each channel to the histogram
    for (uint channel = 0u; channel < 4u; ++channel) {
        const uint bin = channel * numBins + localIndex;
        const uint count = localBins[bin];
        if (count > 0u) {
            uint previous;
            InterlockedAdd(bins[bin], count, previous);
        }
    }
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

    // The panel: the histogram (a 256 x 1 texture of the counts, r g b luminance) over a dark
    // rounded rectangle, drawn into the pass's viewport.
    constexpr const char* kPanelSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct PanelUniforms
{
    float4 panel;   // xy: the panel's size in points, z: the pixels counted
};
VT_DECLARE_QUAD_UNIFORMS(PanelUniforms, u)
VT_DECLARE_QUAD_TEXEL_INPUT0(binTexture)

static const float numBins = 256.0;

// The height of a channel's histogram at x, 0 - 1, interpolating the neighbouring bins. The
// counts are on a logarithmic scale, so tones with few pixels show beside those with most: a bin
// holding all pixels reaches the top, one holding the average count a third of the way up.
float histogram(uint channel, float x)
{
    const float f = clamp(x * numBins - 0.5, 0.0, numBins - 1.0);
    const int i = int(f);
    const int j = min(i + 1, 255);
    const float count = lerp(binTexture.Load(int3(i, 0, 0))[channel], binTexture.Load(int3(j, 0, 0))[channel], frac(f));
    const float average = u.panel.z / numBins;
    return log2(1.0 + 16.0 * count / average) / log2(1.0 + 16.0 * numBins);
}

// the signed distance from a point to a rounded rectangle centred at the origin
float roundedRect(float2 p, float2 halfSize, float radius)
{
    const float2 q = abs(p) - halfSize + radius;
    return length(max(q, float2(0.0, 0.0))) + min(max(q.x, q.y), 0.0) - radius;
}

[shader("vertex")]
QuadVarying panelVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 panelFragment(QuadVarying input) : SV_Target0
{
    const float2 size = u.panel.xy;
    const float2 p = float2(input.uv.x, 1.0 - input.uv.y) * size;   // y up, as the chart reads

    // a dark rounded rectangle with a faint outline
    const float d = roundedRect(p - size * 0.5, size * 0.5, 14.0);
    float3 color = float3(0.06, 0.07, 0.09);
    color = lerp(color, float3(1.0, 1.0, 1.0), saturate(1.0 - abs(d + 1.0)) * 0.1);
    const float alpha = saturate(0.5 - d) * 0.85;

    // the chart, with room at the top for labels and at the bottom for the tone ramp
    const float2 chartMin = float2(16.0, 28.0);
    const float2 chartMax = size - float2(16.0, 40.0);
    const float2 chartSize = chartMax - chartMin;
    const float2 c = (p - chartMin) / chartSize;
    if (c.x >= 0.0 && c.x <= 1.0 && c.y >= 0.0 && c.y <= 1.0) {
        // faint lines dividing the tones into quarters
        const float quarter = abs(frac(c.x * 4.0 + 0.5) - 0.5) * chartSize.x * 0.25;
        color += saturate(1.0 - quarter) * 0.05;

        // the red, green and blue areas added together, white where they overlap
        const float pixel = 1.0 / chartSize.y;
        const float r = saturate((histogram(0u, c.x) - c.y) / pixel);
        const float g = saturate((histogram(1u, c.x) - c.y) / pixel);
        const float b = saturate((histogram(2u, c.x) - c.y) / pixel);
        color += r * float3(0.55, 0.12, 0.16) + g * float3(0.12, 0.5, 0.2) + b * float3(0.12, 0.25, 0.6);

        // the luminance as a white line along its top, of one width whatever its slope
        const float dx = 1.0 / chartSize.x;
        const float slope = (histogram(3u, c.x + dx) - histogram(3u, c.x - dx)) * 0.5 * chartSize.y;
        const float line = abs(histogram(3u, c.x) - c.y) * chartSize.y / sqrt(1.0 + slope * slope);
        color = lerp(color, float3(1.0, 1.0, 1.0), saturate(1.5 - line) * 0.9);
    }

    // the tone ramp under the chart, black to white
    if (c.x >= 0.0 && c.x <= 1.0 && p.y >= 14.0 && p.y <= 20.0) {
        color = float3(c.x, c.x, c.x);
    }
    return float4(color, alpha);
}
)slang";

    class HistogramPanelPass final : public RenderPassShaderQuad
    {
    public:
        HistogramPanelPass(const std::shared_ptr<GraphicsDevice>& device, Texture* bins)
            : RenderPassShaderQuad(device), _bins(bins)
        {
            setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        }

        void setPanel(const float width, const float height, const float pixelCount)
        {
            _uniforms[0] = width;
            _uniforms[1] = height;
            _uniforms[2] = std::max(pixelCount, 1.0f);
        }

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("example-histogram-panel", kPanelSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            setQuadUniformData(_uniforms, sizeof(_uniforms));
            setQuadTextureBinding(0, _bins);
            RenderPassShaderQuad::execute();
        }

    private:
        Texture* _bins;
        float _uniforms[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    };
}

class HistogramExample final : public ExampleApp
{
public:
    HistogramExample() : ExampleApp({.title = "Compute Histogram", .miniStatsPresets = {"compute"}}) {}

protected:
    bool create() override
    {
        _knotAsset = std::make_unique<Asset>("knot", AssetType::CONTAINER, assetPath("models/trefoil-knot.glb"));
        _envAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/product-studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* knotContainer = _knotAsset->resourceAs<ContainerResource>();
        Texture* env = _envAsset->resourceAs<Texture>();
        if (!knotContainer || !env) {
            spdlog::error("histogram needs models/trefoil-knot.glb and the product studio atlas");
            return false;
        }

        // Soft reflections and a little ambient light from the studio; the camera renders no
        // skybox, so the background is black and the floor fades into it.
        scene()->setEnvAtlas(env);
        scene()->setSkyboxIntensity(0.15f);
        scene()->setFogEnabled(true);
        scene()->setFogType(FogType::FOG_LINEAR);
        scene()->setFogColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        scene()->setFogLinear(8.0f, 18.0f);

        _knot = knotContainer->instantiateRenderEntity();
        _knot->setEngine(engine());
        _knot->setLocalPosition(0.0f, 1.9f, 0.0f);
        _knot->setLocalScale(0.8f, 0.8f, 0.8f);
        root()->addChild(_knot);

        _floorMaterial = std::make_shared<StandardMaterial>();
        _floorMaterial->setDiffuse(Color(0.35f, 0.35f, 0.37f, 1.0f));
        _floorMaterial->setGloss(0.4f);
        createPrimitive("plane", _floorMaterial.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(40.0f, 1.0f, 40.0f))
            ->setName("Floor");

        // Three coloured spot lights circling above the knot, shining down on it.
        for (const char* color : {"#ff3da8", "#3dd8ff", "#ffb43d"}) {
            auto* entity = new Entity();
            entity->setName("SpotLight");
            entity->setEngine(engine());
            if (auto* light = static_cast<LightComponent*>(entity->addComponent<LightComponent>())) {
                light->setType(LightType::LIGHTTYPE_SPOT);
                light->setColor(Color().fromString(color));
                light->setIntensity(4.0f);
                light->setRange(20.0f);
                light->setInnerConeAngle(14.0f);
                light->setOuterConeAngle(26.0f);
                light->setCastShadows(true);
                light->setShadowResolution(1024);
                light->setShadowBias(0.2f);
                light->setShadowNormalBias(0.05f);
            }
            root()->addChild(entity);
            _lights.push_back(entity);
        }

        // The camera grabs its scene colour (on the depth layer, after the opaque objects, so the
        // panel drawn later is not part of it): the input of the histogram.
        Entity* cameraEntity = createCamera(Vector3(0.0f, 3.5f, 7.5f));
        _camera = cameraEntity->findComponent<CameraComponent>();
        _camera->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _camera->setToneMapping(TONEMAP_NEUTRAL);
        _camera->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_IMMEDIATE, LAYERID_UI});
        _camera->requestSceneColorMap(true);
        if (CameraControls* controls = addOrbitControls(cameraEntity, Vector3(0.0f, 1.6f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-80.0f, 5.0f));
            controls->setZoomRange(Vector2(3.0f, 15.0f));
            controls->storeResetState();
        }

        if (!device()->supportsCompute()) {
            spdlog::warn("histogram: this device has no compute; no histogram");
            return true;
        }

        // The storage buffer the histogram is written to, 4 bytes a bin, 4 channels.
        VertexBufferOptions bufferOptions;
        bufferOptions.data.assign(kNumBins * kChannels * sizeof(uint32_t), 0);
        _bins = device()->createVertexBuffer(std::make_shared<VertexFormat>(4, true, false),
            static_cast<int>(kNumBins * kChannels), bufferOptions);

        _histogramShader = getOrCreateSlangShaderFromSource(device().get(), "histogram-compute", kHistogramSlang);
        _clearShader = getOrCreateSlangShaderFromSource(device().get(), "histogram-clear", kClearSlang);
        if (!_bins || !_histogramShader || !_clearShader) {
            spdlog::error("histogram: the compute shaders or the storage buffer could not be created");
            return false;
        }

        // Clears the buffer, to avoid the accumulation buildup.
        _clear = std::make_unique<Compute>(device().get(), _clearShader, "HistogramClear");
        _clear->setParameter("bins", _bins);
        _clear->setupDispatchThreads(kNumBins * kChannels);

        // The histogram reads the camera's scene colour map, attached once: each dispatch reads
        // the map the camera rendered most recently.
        _histogram = std::make_unique<Compute>(device().get(), _histogramShader, "ComputeHistogram");
        _histogram->setSceneColorMap(_camera->sceneColorMapHandle());
        _histogram->setParameter("bins", _bins);

        // After the camera renders, so it bins the frame the camera rendered.
        _computePass = std::make_shared<ComputePass>(device(), std::vector<Compute*>{_clear.get(), _histogram.get()});
        _computePass->setName("HistogramPass");
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
            _pixelCount = static_cast<float>(width) * static_cast<float>(height);
        });
        _camera->afterPasses().push_back(_computePass);

        // The panel: the counts in a float texture, drawn by a quad pass after the frame.
        TextureOptions binOptions;
        binOptions.name = "HistogramBins";
        binOptions.width = kNumBins;
        binOptions.height = 1;
        binOptions.format = PixelFormat::PIXELFORMAT_RGBA32F;
        binOptions.mipmaps = false;
        binOptions.minFilter = FilterMode::FILTER_NEAREST;
        binOptions.magFilter = FilterMode::FILTER_NEAREST;
        _binTexture = std::make_shared<Texture>(device().get(), binOptions);
        _binData.assign(kNumBins * 4, 0.0f);
        uploadBins();
        _panel = std::make_shared<HistogramPanelPass>(device(), _binTexture.get());
        _panel->init(nullptr);
        engine()->renderer()->addAppendPass(_panel);

        spdlog::info("histogram: [ ] exposure, , . speed");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_LEFTBRACKET: _exposure = std::max(0.25f, _exposure - 0.25f); break;
        case SDLK_RIGHTBRACKET: _exposure = std::min(3.0f, _exposure + 0.25f); break;
        case SDLK_COMMA: _speed = std::max(0.0f, _speed - 0.25f); break;
        case SDLK_PERIOD: _speed = std::min(3.0f, _speed + 0.25f); break;
        default: return false;
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * _speed;
        scene()->setExposure(_exposure);

        // The lights circle the knot, a third of a turn apart, rising and falling.
        for (size_t i = 0; i < _lights.size(); ++i) {
            const float fi = static_cast<float>(i);
            const float angle = _time * 0.6f + fi * 2.0f * std::numbers::pi_v<float> / 3.0f;
            _lights[i]->setLocalPosition(4.0f * std::cos(angle), 5.5f + std::sin(_time * 0.9f + fi * 2.0f),
                4.0f * std::sin(angle));
            _lights[i]->lookAt(Vector3(0.0f, 1.2f, 0.0f));
            _lights[i]->rotateLocal(90.0f, 0.0f, 0.0f);
        }
        _knot->setLocalEulerAngles(0.0f, _time * 12.0f, 0.0f);

        // The previous frame's histogram, which this waits for, into the panel's texture.
        if (_bins && _binTexture) {
            std::array<uint32_t, kNumBins * kChannels> data{};
            if (_bins->read(0, sizeof(data), data.data())) {
                for (uint32_t bin = 0; bin < kNumBins; ++bin) {
                    for (uint32_t channel = 0; channel < kChannels; ++channel) {
                        _binData[bin * 4 + channel] = static_cast<float>(data[channel * kNumBins + bin]);
                    }
                }
                uploadBins();
            }
        }

        // The panel in the bottom right corner: 20 points in, at most 570 wide, 255 tall.
        if (_panel) {
            const auto [width, height] = device()->size();
            const float fw = static_cast<float>(width);
            const float fh = static_cast<float>(height);
            const float panelWidth = std::min(570.0f, fw - 40.0f);
            const float panelHeight = 255.0f;
            _panel->setViewport(Vector4(fw - 20.0f - panelWidth, fh - 20.0f - panelHeight, panelWidth, panelHeight));
            _panel->setScissor(_panel->viewport());
            _panel->setPanel(panelWidth, panelHeight, _pixelCount);
        }
    }

    void destroy() override
    {
        // The passes are the camera's and the renderer's; the computes and the buffer hold device
        // resources.
        if (_panel) {
            engine()->renderer()->removeAppendPass(_panel);
            _panel.reset();
        }
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
    void uploadBins()
    {
        _binTexture->setLevelData(0, reinterpret_cast<const uint8_t*>(_binData.data()), _binData.size() * sizeof(float));
        _binTexture->upload();
    }

    std::unique_ptr<Asset> _knotAsset;
    std::unique_ptr<Asset> _envAsset;
    Entity* _knot = nullptr;
    std::shared_ptr<StandardMaterial> _floorMaterial;
    std::vector<Entity*> _lights;
    CameraComponent* _camera = nullptr;

    std::shared_ptr<VertexBuffer> _bins;
    std::shared_ptr<Shader> _histogramShader;
    std::shared_ptr<Shader> _clearShader;
    std::unique_ptr<Compute> _clear;
    std::unique_ptr<Compute> _histogram;
    std::shared_ptr<ComputePass> _computePass;
    std::shared_ptr<Texture> _binTexture;
    std::vector<float> _binData;
    std::shared_ptr<HistogramPanelPass> _panel;

    float _pixelCount = 1.0f;
    float _exposure = 1.0f;
    float _speed = 1.0f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(HistogramExample)
