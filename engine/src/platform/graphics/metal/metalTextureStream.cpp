// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Triple-buffered texture streaming for real-time data ingestion.
//
#include "metalTextureStream.h"

#include <cassert>
#include <string>
#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    MetalTextureStream::MetalTextureStream(MTL::Device* device, const Descriptor& desc)
        : _device(device), _desc(desc)
    {
        assert(device && "MetalTextureStream requires a valid MTL::Device");
        assert(desc.width > 0 && desc.height > 0 && "Texture dimensions must be positive");

        // Create texture descriptor for all 3 slots.
        // StorageModeShared: Apple Silicon unified memory, no blit needed.
        // WriteCombined: bypasses CPU cache for write-only streaming pattern.
        auto* texDesc = MTL::TextureDescriptor::texture2DDescriptor(
            desc.format, desc.width, desc.height, /*mipmapped=*/false);

        texDesc->setStorageMode(MTL::StorageModeShared);
        texDesc->setUsage(MTL::TextureUsageShaderRead);

        if (desc.writeCombined) {
            texDesc->setCpuCacheMode(MTL::CPUCacheModeWriteCombined);
        }

        for (int i = 0; i < kNumSlots; ++i) {
            _textures[i] = device->newTexture(texDesc);
            if (_textures[i]) {
                auto labelStr = std::string(desc.label) + "_" + std::to_string(i);
                _textures[i]->setLabel(
                    NS::String::string(labelStr.c_str(), NS::UTF8StringEncoding));
            }
        }

        texDesc->release();

        // Semaphore with initial count = kNumSlots: all 3 slots are initially free.
        _slotSemaphore = dispatch_semaphore_create(kNumSlots);

        spdlog::info("[MetalTextureStream] Created: {}x{} {} ({} slots, {:.1f} MB total)",
            desc.width, desc.height,
            desc.format == MTL::PixelFormatBGRA8Unorm ? "BGRA8" :
            desc.format == MTL::PixelFormatRGBA8Unorm ? "RGBA8" :
            desc.format == MTL::PixelFormatRGBA16Float ? "RGBA16F" :
            desc.format == MTL::PixelFormatR32Float ? "R32F" : "other",
            kNumSlots,
            static_cast<double>(kNumSlots * desc.width * desc.height * 4) / (1024.0 * 1024.0));
    }

    MetalTextureStream::~MetalTextureStream()
    {
        for (auto& tex : _textures) {
            if (tex) {
                tex->release();
                tex = nullptr;
            }
        }

        spdlog::info("[MetalTextureStream] Destroyed: {} frames published, {} dropped",
            _publishCount.load(std::memory_order_relaxed),
            _dropCount.load(std::memory_order_relaxed));
    }

    // ── Producer API ───────────────────────────────────────────────────

    MTL::Texture* MetalTextureStream::beginWrite()
    {
        // Block if all 3 slots are in-flight (GPU back-pressure).
        // This matches MetalUniformRingBuffer::beginFrame().
        dispatch_semaphore_wait(_slotSemaphore, DISPATCH_TIME_FOREVER);

        const int idx = _writeIndex.load(std::memory_order_acquire);
        return _textures[idx];
    }

    void MetalTextureStream::writeRegion(const void* data, size_t bytesPerRow,
                                          MTL::Region region, uint32_t mipLevel)
    {
        assert(data && "writeRegion: data must not be null");

        const int idx = _writeIndex.load(std::memory_order_acquire);
        _textures[idx]->replaceRegion(region, mipLevel, 0, data, bytesPerRow, 0);
    }

    void MetalTextureStream::endWrite()
    {
        // Atomically swap write and ready slots.
        // If the consumer hasn't picked up the previous ready frame, it gets
        // overwritten ("latest-frame-wins" policy). Track that as a drop.
        const int oldWrite = _writeIndex.load(std::memory_order_acquire);
        const int oldReady = _readyIndex.load(std::memory_order_acquire);

        _readyIndex.store(oldWrite, std::memory_order_release);
        _writeIndex.store(oldReady, std::memory_order_release);

        _publishCount.fetch_add(1, std::memory_order_relaxed);
        _hasPublished = true;
    }

    void MetalTextureStream::publishExternal(MTL::Texture* externalTexture)
    {
        assert(externalTexture && "publishExternal: texture must not be null");
        _externalReady.store(externalTexture, std::memory_order_release);
        _publishCount.fetch_add(1, std::memory_order_relaxed);
        _hasPublished = true;
    }

    // ── Consumer API ───────────────────────────────────────────────────

    MTL::Texture* MetalTextureStream::acquireForRead()
    {
        // Check for externally-published texture first (CVMetalTexture path).
        auto* ext = _externalReady.exchange(nullptr, std::memory_order_acq_rel);
        if (ext) {
            return ext;
        }

        if (!_hasPublished) {
            return nullptr;
        }

        // Atomically swap ready and read slots.
        const int oldReady = _readyIndex.load(std::memory_order_acquire);
        const int oldRead  = _readIndex.load(std::memory_order_acquire);

        _readIndex.store(oldReady, std::memory_order_release);
        _readyIndex.store(oldRead, std::memory_order_release);

        return _textures[_readIndex.load(std::memory_order_acquire)];
    }

    void MetalTextureStream::endFrame(MTL::CommandBuffer* commandBuffer)
    {
        assert(commandBuffer && "endFrame: commandBuffer must not be null");

        // Register completion handler: when GPU finishes this frame's command
        // buffer, signal the semaphore to release one slot for the producer.
        // Same pattern as MetalUniformRingBuffer::endFrame().
        dispatch_semaphore_t sem = _slotSemaphore;
        commandBuffer->addCompletedHandler(^(MTL::CommandBuffer*) {
            dispatch_semaphore_signal(sem);
        });
    }

}  // namespace visutwin::canvas
