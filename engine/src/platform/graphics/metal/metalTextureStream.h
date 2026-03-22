// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Triple-buffered texture streaming for real-time data ingestion.
//
// Provides lock-free single-producer single-consumer texture rotation
// for tear-free real-time intraoperative display of endoscope video
// and simulation feeds.
//
// Uses dispatch_semaphore_t for GPU back-pressure, consistent with
// MetalUniformRingBuffer. StorageModeShared + WriteCombined for
// optimal Apple Silicon streaming throughput.
//
// Usage:
//   MetalTextureStream stream(device, {1920, 1080});
//
//   // Producer (any thread):
//   auto* tex = stream.beginWrite();
//   stream.writeRegion(pixels, bytesPerRow, region);
//   stream.endWrite();
//
//   // Consumer (render thread):
//   auto* readTex = stream.acquireForRead();
//   metalTexture->setExternalTexture(readTex);
//   // ... render ...
//   stream.endFrame(commandBuffer);
//
#pragma once

#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>
#include <array>
#include <atomic>
#include <cstdint>

namespace visutwin::canvas
{
    /**
     * Triple-buffered texture streaming for real-time data ingestion.
     *
     * Three MTL::Texture instances rotate through Write / Ready / Read states:
     *   - Write:  CPU actively writes new pixel data
     *   - Ready:  Most recent completed frame, waiting for GPU pickup
     *   - Read:   GPU reads from this texture during the current render pass
     *
     * Critical invariant: CPU never writes to a texture the GPU is currently
     * reading. The Ready slot decouples producer from consumer.
     *
     * Thread safety: Single producer, single consumer. The producer calls
     * beginWrite/writeRegion/endWrite; the consumer calls acquireForRead/endFrame.
     * For multiple producers, wrap beginWrite/endWrite in a mutex.
     */
    class MetalTextureStream
    {
    public:
        static constexpr int kNumSlots = 3;

        struct Descriptor
        {
            uint32_t width  = 1920;
            uint32_t height = 1080;
            MTL::PixelFormat format = MTL::PixelFormatBGRA8Unorm;
            bool writeCombined = true;   ///< CPU write-only optimization (bypass cache)
            const char* label = "TextureStream";
        };

        MetalTextureStream(MTL::Device* device, const Descriptor& desc);
        ~MetalTextureStream();

        // Non-copyable, non-movable
        MetalTextureStream(const MetalTextureStream&) = delete;
        MetalTextureStream& operator=(const MetalTextureStream&) = delete;
        MetalTextureStream(MetalTextureStream&&) = delete;
        MetalTextureStream& operator=(MetalTextureStream&&) = delete;

        // ── Producer API (producer thread) ─────────────────────────────

        /// Begin writing to the next available texture slot.
        /// Returns the texture to write into.
        /// Blocks if all 3 slots are in-flight (GPU back-pressure).
        MTL::Texture* beginWrite();

        /// Write pixel data into a region of the current write texture.
        /// Convenience wrapper around MTL::Texture::replaceRegion().
        /// Must be called between beginWrite() and endWrite().
        void writeRegion(const void* data, size_t bytesPerRow,
                         MTL::Region region, uint32_t mipLevel = 0);

        /// Finish writing. The texture becomes the new "ready" frame.
        /// Atomically swaps write and ready slots.
        void endWrite();

        /// Publish an externally-owned texture (e.g., from CVMetalTextureCache).
        /// The stream does NOT own this texture; caller must keep it alive
        /// until the next publishExternal() or endWrite() call.
        void publishExternal(MTL::Texture* externalTexture);

        // ── Consumer API (render thread) ───────────────────────────────

        /// Get the most recently completed texture for GPU reading.
        /// Atomically swaps ready and read slots.
        /// Returns nullptr if no frame has been published yet.
        MTL::Texture* acquireForRead();

        /// Register GPU completion on the command buffer so that the
        /// read texture slot can be recycled. Must be called on the LAST
        /// command buffer committed per frame.
        /// Same pattern as MetalUniformRingBuffer::endFrame().
        void endFrame(MTL::CommandBuffer* commandBuffer);

        // ── Accessors ──────────────────────────────────────────────────

        [[nodiscard]] uint32_t width() const { return _desc.width; }
        [[nodiscard]] uint32_t height() const { return _desc.height; }
        [[nodiscard]] MTL::PixelFormat format() const { return _desc.format; }

        /// Total number of frames published by the producer.
        [[nodiscard]] uint64_t framesPublished() const { return _publishCount.load(std::memory_order_relaxed); }

        /// Number of frames dropped (overwritten in Ready before GPU consumed them).
        [[nodiscard]] uint64_t framesDropped() const { return _dropCount.load(std::memory_order_relaxed); }

        /// True if at least one frame has been published.
        [[nodiscard]] bool hasNewFrame() const { return _hasPublished; }

    private:
        MTL::Device* _device;
        Descriptor _desc;

        std::array<MTL::Texture*, kNumSlots> _textures{};
        dispatch_semaphore_t _slotSemaphore = nullptr;  ///< count = kNumSlots

        // Atomic indices for lock-free SPSC rotation.
        // Each index is in [0, kNumSlots). All three are always distinct.
        std::atomic<int> _writeIndex{0};
        std::atomic<int> _readyIndex{1};
        std::atomic<int> _readIndex{2};

        // External texture path (CVMetalTexture zero-copy).
        // When non-null, acquireForRead() returns this instead of rotating.
        std::atomic<MTL::Texture*> _externalReady{nullptr};

        // Statistics
        std::atomic<uint64_t> _publishCount{0};
        std::atomic<uint64_t> _dropCount{0};

        bool _hasPublished = false;  ///< Consumer-side flag: has any frame been published?
    };

}  // namespace visutwin::canvas
