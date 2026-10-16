// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.10.2026
//
#include "autoExposureEffect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

#include <spdlog/spdlog.h>

#include "colorTarget.h"
#include "core/math/vector4.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/componentSystem.h"
#include "framework/engine.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "renderPassShaderQuad.h"
#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr int kMeterWidth = 128;
        constexpr int kMeterHeight = 64;
        constexpr int kBinTexels = AutoExposureEffect::kBins / 4;

        // The log2 of middle grey, the brightness the average is shown at before compensation.
        const float kLog2MiddleGrey = std::log2(0.18f);
        // How far the histogram's range reaches beyond the brightness the exposure limits show
        // as middle grey, in EV, so what the average leaves out is still counted apart from it.
        constexpr float kRangeMargin = 4.0f;

        // The compose chunk of the debug view: the scene at the camera's exposure with the
        // histogram along the bottom. The bins are 16 float4 uniforms and the markers one.
        std::string debugChunk()
        {
            std::string chunk = "float aeBin(int bin)\n{\n    const int row = bin / 4;\n    float4 r = aeHistogram0;\n";
            for (int i = 1; i < kBinTexels; ++i) {
                chunk += "    if (row == " + std::to_string(i) + ") { r = aeHistogram" + std::to_string(i) + "; }\n";
            }
            chunk += "    return r[bin % 4];\n}\n";
            chunk += R"(
float3 aeToneMap(float3 c)
{
    const uint mode = (vtFeatureMask0 >> kComposeSwitchToneMapShift) & 0xFu;
    if (mode == 1u) { return toneMapFilmic(c, u.exposure); }
    if (mode == 2u) { return toneMapHejl(c, u.exposure); }
    if (mode == 3u) { return toneMapAces(c, u.exposure); }
    if (mode == 4u) { return toneMapAces2(c, u.exposure); }
    if (mode == 5u) { return toneMapNeutral(c, u.exposure); }
    if (mode == 6u) { return c; }
    return c * u.exposure;
}

// The bins the average includes bright, the average in yellow, the brightness the camera
// exposes as middle grey in blue.
float3 debugAutoexposure()
{
    const float2 uv = composeDebugUv;
    float3 color = aeToneMap(max(float3(0.0, 0.0, 0.0), sceneTexture.SampleLevel(uv, 0.0).rgb));

    // The panel along the bottom, its vertical position growing upwards.
    const float2 panel = (float2(uv.x, 1.0 - uv.y) - float2(0.05, 0.04)) / float2(0.9, 0.22);
    if (all(panel >= float2(0.0, 0.0)) && all(panel <= float2(1.0, 1.0))) {
        const int bin = int(panel.x * 63.0 + 0.5);
        const float height = aeBin(bin);
        const bool included = panel.x >= aeMarkers.x && panel.x <= aeMarkers.y;
        color = panel.y <= height ? (included ? float3(0.85, 0.85, 0.85) : float3(0.3, 0.3, 0.3)) : color * 0.15;

        // The markers, two pixels wide.
        const float halfWidth = sceneTextureSize.z / 0.9;
        if (abs(panel.x - aeMarkers.z) < halfWidth) {
            color = float3(1.0, 0.75, 0.1);
        }
        if (abs(panel.x - aeMarkers.w) < halfWidth) {
            color = float3(0.2, 0.7, 1.0);
        }
    }
    return color;
}
)";
            return chunk;
        }
    }

    float meterHistogram(const float* histogram, const size_t bins, const float lowPercent, const float highPercent)
    {
        double total = 0.0;
        for (size_t i = 0; i < bins; ++i) {
            total += histogram[i];
        }
        const double low = total * lowPercent;
        const double high = total * highPercent;
        double start = 0.0;
        double sum = 0.0;
        double weight = 0.0;
        for (size_t i = 0; i < bins; ++i) {
            const double end = start + histogram[i];
            const double part = std::min(end, high) - std::max(start, low);
            if (part > 0.0) {
                sum += part * static_cast<double>(i);
                weight += part;
            }
            start = end;
        }
        return weight > 0.0 ? static_cast<float>(sum / weight) : -1.0f;
    }

    // One of the three passes: the variant of auto-exposure.slang, its input, the effect's
    // uniform block, and what runs once it has rendered.
    class AutoExposureEffect::Pass final : public RenderPassShaderQuad
    {
    public:
        Pass(const std::shared_ptr<GraphicsDevice>& device, std::string variant, const Uniforms* uniforms)
            : RenderPassShaderQuad(device), _variant(std::move(variant)), _uniforms(uniforms)
        {
            _name = "RenderPassAutoExposure-" + _variant;
        }

        void setInput(Texture* texture) { _input = texture; }
        void setOnAfter(std::function<void()> onAfter) { _onAfter = std::move(onAfter); }

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangShader("auto-exposure", _variant);
            }
        }

        void execute() override
        {
            prepareShaders();
            if (!shader() || !_input) {
                return;
            }
            setQuadTextureBinding(0, _input);
            setQuadUniforms(*_uniforms);
            RenderPassShaderQuad::execute();
        }

        void after() override
        {
            if (_onAfter) {
                _onAfter();
            }
        }

    private:
        std::string _variant;
        const Uniforms* _uniforms = nullptr;
        Texture* _input = nullptr;
        std::function<void()> _onAfter;
    };

    AutoExposureEffect::AutoExposureEffect()
        : CameraFrameEffect("autoExposure", CameraFrameEffectOptions{.slang = debugChunk(),
              .debugViews = {"autoexposure"}, .requiredResources = {FrameResource::SceneColorHalf}})
    {
        // The debug view's uniforms exist from the start, so the chunk always finds them.
        update();
    }

    AutoExposureEffect::~AutoExposureEffect() = default;

    bool AutoExposureEffect::active() const
    {
        if (!enabled()) {
            return false;
        }
        const CameraComponent* camera = this->camera();
        if (!camera) {
            return true;
        }
        int toneMapping = camera->toneMapping();
        if (toneMapping == TONEMAP_INHERIT) {
            const Engine* engine = camera->system() ? camera->system()->engine() : nullptr;
            toneMapping = engine && engine->scene() ? engine->scene()->toneMapping() : TONEMAP_LINEAR;
        }
        return toneMapping != TONEMAP_NONE;
    }

    void AutoExposureEffect::createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes)
    {
        GraphicsDevice* device = resources.device.get();
        if (!device || !resources.sceneColorHalf) {
            return;
        }
        // The counts are floats: a target of 32-bit floats must be renderable.
        if (!device->textureFloatRenderable()) {
            spdlog::warn("AutoExposureEffect: the device cannot render to 32-bit float targets; no metering");
            return;
        }
        _source = resources.sceneColorHalf;

        auto meter = createColorTarget(device, "AutoExposureMeter", PixelFormat::PIXELFORMAT_RGBA8,
            kMeterWidth, kMeterHeight);
        auto rows = createColorTarget(device, "AutoExposureRows", PixelFormat::PIXELFORMAT_RGBA32F,
            kBinTexels, kMeterHeight);
        auto histogram = createColorTarget(device, "AutoExposureHistogram", PixelFormat::PIXELFORMAT_RGBA32F,
            kBinTexels, 1);
        _meter = meter.texture;
        _rows = rows.texture;
        _histogramTexture = histogram.texture;
        for (const auto& texture : {_meter, _rows, _histogramTexture}) {
            texture->setMinFilter(FilterMode::FILTER_NEAREST);
            texture->setMagFilter(FilterMode::FILTER_NEAREST);
        }

        // Every pass renders every texel of its fixed-size target: nothing to clear.
        _passes[0] = std::make_shared<Pass>(resources.device, "meter", &_uniforms);
        _passes[0]->init(meter.target);
        _passes[0]->setInput(_source);
        _passes[1] = std::make_shared<Pass>(resources.device, "rows", &_uniforms);
        _passes[1]->init(rows.target);
        _passes[1]->setInput(_meter.get());
        _passes[2] = std::make_shared<Pass>(resources.device, "sum", &_uniforms);
        _passes[2]->init(histogram.target);
        _passes[2]->setInput(_rows.get());
        // The histogram is read back once the pass rendering it has ended.
        _passes[2]->setOnAfter([this] { readHistogram(); });

        for (const auto& pass : _passes) {
            passes.postResolve.push_back(pass);
        }
    }

    void AutoExposureEffect::destroyPasses()
    {
        _passes = {};
        _meter.reset();
        _rows.reset();
        _histogramTexture.reset();
        _source = nullptr;
        // A read still in flight is let go of: its answer would be for passes that are gone.
        _alive = std::make_shared<int>(0);
        _reading = false;
    }

    void AutoExposureEffect::update()
    {
        _minEvFrozen = std::min(minEv, maxEv);
        _maxEvFrozen = std::max(minEv, maxEv);
        _lowFrozen = std::clamp(lowPercent, 0.0f, 0.99f);
        _highFrozen = std::clamp(highPercent, _lowFrozen + 0.01f, 1.0f);
        _uniforms.params[0] = std::clamp(centerWeight, 0.0f, 1.0f);

        for (int i = 0; i < kBinTexels; ++i) {
            setUniform("aeHistogram" + std::to_string(i),
                Vector4(_bars[i * 4], _bars[i * 4 + 1], _bars[i * 4 + 2], _bars[i * 4 + 3]));
        }
        setUniform("aeMarkers", Vector4(_markers[0], _markers[1], _markers[2], _markers[3]));
    }

    void AutoExposureEffect::frameUpdate(const CameraFrameEffectContext& context)
    {
        const CameraComponent* cameraComponent = camera();
        const Camera* camera = cameraComponent ? cameraComponent->camera() : nullptr;
        const Engine* engine = cameraComponent && cameraComponent->system() ? cameraComponent->system()->engine() : nullptr;
        const Scene* scene = engine ? engine->scene().get() : nullptr;
        if (!scene) {
            return;
        }

        // The camera's own exposure, which the adaptation scales.
        const float baseExposure = scene->physicalUnits() && camera ? camera->physicalExposure() : scene->exposure();
        const float log2Base = std::log2(std::max(baseExposure, 1e-30f));

        // The brightness the average of the screen is shown at.
        const float key = kLog2MiddleGrey + compensation;

        // Towards the exposure that shows the measured average at the key brightness, within the
        // limits; faster as the scene gets brighter. It advances by the world's time, so it
        // stops while the world is paused.
        if (_hasAverage) {
            const float target = std::clamp(key - _averageLog2 - log2Base, _minEvFrozen, _maxEvFrozen);
            if (_snap) {
                _ev = target;
                _snap = false;
            } else {
                const float delta = target - _ev;
                const float speed = std::max(delta < 0.0f ? speedDarkToLight : speedLightToDark, 0.0f);
                _ev += delta * (1.0f - std::exp(-context.dt * speed));
            }
        }
        _ev = std::clamp(_ev, _minEvFrozen, _maxEvFrozen);
        _exposureScale = std::exp2(_ev);
        _exposure = baseExposure * _exposureScale;

        // The range the histogram measures this frame: from the brightness the highest exposure
        // shows as middle grey to the one the lowest does, and a margin beyond either.
        _rangeMin = key - log2Base - _maxEvFrozen - kRangeMargin;
        _rangeMax = key - log2Base - _minEvFrozen + kRangeMargin;
        if (_source) {
            _uniforms.source[0] = static_cast<float>(_source->width());
            _uniforms.source[1] = static_cast<float>(_source->height());
        }
        _uniforms.source[2] = _rangeMin;
        _uniforms.source[3] = 1.0f / std::max(_rangeMax - _rangeMin, 1e-6f);

        // The debug view marks the brightness the camera exposes as middle grey along the range
        // the shown histogram was measured over.
        _markers[3] = _barsMax > _barsMin ? (key - std::log2(_exposure) - _barsMin) / (_barsMax - _barsMin) : 0.0f;
    }

    void AutoExposureEffect::reset()
    {
        ++_measurement;
        _hasAverage = false;
        _snap = true;
    }

    void AutoExposureEffect::readHistogram()
    {
        // One read at a time: the next once the last has arrived.
        if (_reading || !_histogramTexture) {
            return;
        }
        const float rangeMin = _rangeMin;
        const float rangeMax = _rangeMax;
        const int tag = _measurement;
        std::weak_ptr<int> alive = _alive;
        const bool queued = _histogramTexture->readAsync(
            [this, alive, rangeMin, rangeMax, tag](const uint8_t* data, const size_t size) {
                if (alive.expired()) {
                    return;
                }
                _reading = false;
                if (size < sizeof(float) * kBins) {
                    return;
                }
                std::array<float, kBins> bins{};
                std::memcpy(bins.data(), data, sizeof(float) * kBins);
                measure(bins.data(), rangeMin, rangeMax, tag);
            });
        // A device that cannot read back measures nothing.
        _reading = queued;
    }

    void AutoExposureEffect::measure(const float* histogram, const float rangeMin, const float rangeMax, const int tag)
    {
        // Measured before a reset: the view before the cut.
        if (tag != _measurement) {
            return;
        }
        ++_histogramsReceived;
        std::copy(histogram, histogram + kBins, _histogram.begin());

        constexpr float lastBin = kBins - 1;
        const float position = meterHistogram(histogram, kBins, _lowFrozen, _highFrozen);
        if (position >= 0.0f) {
            _averageLog2 = rangeMin + position / lastBin * (rangeMax - rangeMin);
            _hasAverage = true;
        }

        // The debug view: the bins relative to the largest, the fractions the average is taken
        // between and the average, as positions along the histogram.
        float largest = 1e-6f;
        double total = 0.0;
        for (int i = 0; i < kBins; ++i) {
            largest = std::max(largest, histogram[i]);
            total += histogram[i];
        }
        for (int i = 0; i < kBins; ++i) {
            _bars[i] = histogram[i] / largest;
        }
        int lowBin = -1;
        int highBin = kBins - 1;
        double start = 0.0;
        for (int i = 0; i < kBins; ++i) {
            const double end = start + histogram[i];
            if (lowBin < 0 && end > total * _lowFrozen) {
                lowBin = i;
            }
            if (end >= total * _highFrozen) {
                highBin = i;
                break;
            }
            start = end;
        }
        _markers[0] = static_cast<float>(std::max(lowBin, 0)) / lastBin;
        _markers[1] = static_cast<float>(highBin) / lastBin;
        _markers[2] = position >= 0.0f ? position / lastBin : 0.0f;
        _barsMin = rangeMin;
        _barsMax = rangeMax;
    }
}
