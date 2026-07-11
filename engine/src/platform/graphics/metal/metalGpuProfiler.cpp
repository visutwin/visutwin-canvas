// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "metalGpuProfiler.h"

#include <cstring>

#include <spdlog/spdlog.h>

namespace visutwin::canvas::gpu
{
    namespace
    {
        struct CounterResultTimestamp
        {
            uint64_t timestamp;
        };

        MTL::CounterSet* findTimestampCounterSet(MTL::Device* device)
        {
            NS::Array* counterSets = device->counterSets();
            if (!counterSets) {
                return nullptr;
            }
            // MTLCommonCounterSetTimestamp resolves to the literal "timestamp"
            // (comparing by value avoids linking the framework extern constant,
            // which metal-cpp only materializes in the *_PRIVATE_IMPLEMENTATION TU).
            for (NS::UInteger i = 0; i < counterSets->count(); ++i) {
                auto* counterSet = static_cast<MTL::CounterSet*>(counterSets->object(i));
                if (counterSet && counterSet->name() &&
                    strcmp(counterSet->name()->utf8String(), "timestamp") == 0) {
                    return counterSet;
                }
            }
            return nullptr;
        }
    }

    std::shared_ptr<MetalGpuProfiler> MetalGpuProfiler::create(MTL::Device* device)
    {
        if (!device || !device->supportsCounterSampling(MTL::CounterSamplingPointAtStageBoundary)) {
            spdlog::info("MetalGpuProfiler: stage-boundary counter sampling unsupported — profiler disabled");
            return nullptr;
        }
        auto profiler = std::shared_ptr<MetalGpuProfiler>(new MetalGpuProfiler(device));
        if (!profiler->init()) {
            return nullptr;
        }
        return profiler;
    }

    MetalGpuProfiler::MetalGpuProfiler(MTL::Device* device)
        : _device(device)
    {
    }

    MetalGpuProfiler::~MetalGpuProfiler()
    {
        for (auto& slot : _slots) {
            if (slot.sampleBuffer) {
                slot.sampleBuffer->release();
                slot.sampleBuffer = nullptr;
            }
        }
    }

    bool MetalGpuProfiler::init()
    {
        MTL::CounterSet* timestampSet = findTimestampCounterSet(_device);
        if (!timestampSet) {
            spdlog::info("MetalGpuProfiler: no timestamp counter set — profiler disabled");
            return false;
        }

        auto* descriptor = MTL::CounterSampleBufferDescriptor::alloc()->init();
        descriptor->setCounterSet(timestampSet);
        descriptor->setStorageMode(MTL::StorageModeShared);
        descriptor->setSampleCount(MAX_PASSES * SAMPLES_PER_PASS);

        bool ok = true;
        for (auto& slot : _slots) {
            NS::Error* error = nullptr;
            slot.sampleBuffer = _device->newCounterSampleBuffer(descriptor, &error);
            if (!slot.sampleBuffer) {
                spdlog::warn("MetalGpuProfiler: counter sample buffer creation failed: {}",
                    error ? error->localizedDescription()->utf8String() : "unknown");
                ok = false;
                break;
            }
            slot.passNames.reserve(MAX_PASSES);
        }
        descriptor->release();

        if (ok) {
            _device->sampleTimestamps(&_baseCpuTimestamp, &_baseGpuTimestamp);
        }
        return ok;
    }

    void MetalGpuProfiler::beginFrame()
    {
        if (!_enabled) {
            return;
        }

        _frameIndex++;
        _currentSlot = static_cast<int>(_frameIndex % NUM_SLOTS);

        // The slot we're about to reuse holds samples from NUM_SLOTS frames ago —
        // resolve them before overwriting (the GPU has long finished that frame).
        resolveSlot(_currentSlot);

        auto& slot = _slots[_currentSlot];
        slot.passNames.clear();
        slot.passCount = 0;
    }

    void MetalGpuProfiler::attachToRenderPass(MTL::RenderPassDescriptor* passDescriptor,
        const std::string& name)
    {
        if (!_enabled || !passDescriptor) {
            return;
        }
        auto& slot = _slots[_currentSlot];
        if (slot.passCount >= MAX_PASSES) {
            return;  // budget exhausted this frame — remaining passes untimed
        }

        auto* attachment = passDescriptor->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(slot.sampleBuffer);
        attachment->setStartOfVertexSampleIndex(
            static_cast<NS::UInteger>(slot.passCount * SAMPLES_PER_PASS));
        attachment->setEndOfVertexSampleIndex(MTL::CounterDontSample);
        attachment->setStartOfFragmentSampleIndex(MTL::CounterDontSample);
        attachment->setEndOfFragmentSampleIndex(
            static_cast<NS::UInteger>(slot.passCount * SAMPLES_PER_PASS + 1));

        slot.passNames.push_back(name.empty() ? "pass" : name);
        slot.passCount++;
    }

    void MetalGpuProfiler::resolveSlot(const int slotIndex)
    {
        auto& slot = _slots[slotIndex];
        if (slot.passCount == 0) {
            return;
        }

        // Correlate GPU ticks with CPU nanoseconds for the conversion factor.
        MTL::Timestamp cpuNow = 0, gpuNow = 0;
        _device->sampleTimestamps(&cpuNow, &gpuNow);
        const double cpuDelta = static_cast<double>(cpuNow - _baseCpuTimestamp);
        const double gpuDelta = static_cast<double>(gpuNow - _baseGpuTimestamp);
        const double nsPerTick = (gpuDelta > 0.0) ? cpuDelta / gpuDelta : 1.0;

        NS::Data* data = slot.sampleBuffer->resolveCounterRange(
            NS::Range::Make(0, static_cast<NS::UInteger>(slot.passCount * SAMPLES_PER_PASS)));
        if (!data) {
            slot.passCount = 0;
            return;
        }

        const auto* samples = static_cast<const CounterResultTimestamp*>(data->bytes());
        _passTimings.clear();
        _frameMilliseconds = 0.0;
        for (int i = 0; i < slot.passCount; ++i) {
            const uint64_t start = samples[i * SAMPLES_PER_PASS + 0].timestamp;
            const uint64_t end = samples[i * SAMPLES_PER_PASS + 1].timestamp;
            // MTLCounterErrorValue marks samples the GPU could not take.
            if (start == static_cast<uint64_t>(MTL::CounterErrorValue) ||
                end == static_cast<uint64_t>(MTL::CounterErrorValue) || end < start) {
                _passTimings.push_back({slot.passNames[static_cast<size_t>(i)], 0.0});
                continue;
            }
            const double ms = static_cast<double>(end - start) * nsPerTick / 1.0e6;
            _passTimings.push_back({slot.passNames[static_cast<size_t>(i)], ms});
            _frameMilliseconds += ms;
        }

        slot.passCount = 0;
    }
}
