// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026.
//
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace visutwin::canvas
{
    /**
     * The bookkeeping of one frame's region of a matrix-palette ring: where each palette
     * goes, which draws can share one, and how much the frame asked for. No GPU types —
     * the ring that owns the memory (MetalPaletteRingBuffer) copies the bytes; this
     * decides the offsets, so the policy can be tested without a device.
     *
     * Two rules:
     *
     *  - A palette is uploaded once per frame, not once per draw. A skin is drawn by the
     *    forward pass and by every shadow pass that sees it, and its palette is the same
     *    bytes each time. A caller that can name the contents — a nonzero version, taken
     *    from GraphicsDevice::nextPaletteVersion() each time the palette is rewritten —
     *    gets the offset of the copy already made this frame. Version 0 names nothing
     *    and is never shared.
     *
     *  - The region GROWS. A frame that asks for more than fits is counted in full
     *    (requestedBytes), and wantedRegionSize() is what the owner reallocates to at
     *    the next frame boundary, as the per-draw uniform rings do.
     */
    class PaletteFrameAllocator
    {
    public:
        struct Allocation
        {
            // Offset inside the frame region, or nothing when the palette did not fit.
            std::optional<size_t> offset;
            // True when the caller has to copy the bytes: a first use that fitted.
            bool isNew = false;
        };

        PaletteFrameAllocator(const size_t regionSize, const size_t alignment)
            : _regionSize(regionSize), _alignment(alignment)
        {
        }

        /// A new frame: nothing allocated, nothing shared, nothing requested.
        void beginFrame()
        {
            _writeOffset = 0;
            _requestedBytes = 0;
            _shared.clear();
        }

        Allocation allocate(const size_t size, const uint64_t contentVersion)
        {
            if (contentVersion != 0) {
                if (const auto found = _shared.find(contentVersion); found != _shared.end()) {
                    // Also when it did NOT fit: it was counted once, on its first use.
                    return {found->second, false};
                }
            }

            const size_t alignedSize = alignUp(size);
            _requestedBytes += alignedSize;

            Allocation result;
            if (_writeOffset + alignedSize <= _regionSize) {
                result.offset = _writeOffset;
                result.isNew = true;
                _writeOffset += alignedSize;
            }
            if (contentVersion != 0) {
                _shared.emplace(contentVersion, result.offset);
            }
            return result;
        }

        /// Whether this frame asked for more than the region holds.
        [[nodiscard]] bool overflowed() const { return _requestedBytes > _regionSize; }

        /// Bytes this frame asked for, including what did not fit.
        [[nodiscard]] size_t requestedBytes() const { return _requestedBytes; }

        /// The region size that would have held this frame: at least double, so a scene
        /// that is still growing does not reallocate every frame.
        [[nodiscard]] size_t wantedRegionSize() const
        {
            return std::max(alignUp(_requestedBytes), _regionSize * 2);
        }

        /// The owner reallocated; the frame in progress starts over against the new size.
        void setRegionSize(const size_t regionSize)
        {
            _regionSize = regionSize;
            beginFrame();
        }

        [[nodiscard]] size_t regionSize() const { return _regionSize; }
        [[nodiscard]] size_t writeOffset() const { return _writeOffset; }

    private:
        [[nodiscard]] size_t alignUp(const size_t value) const
        {
            return (value + _alignment - 1) / _alignment * _alignment;
        }

        size_t _regionSize;
        size_t _alignment;
        size_t _writeOffset = 0;
        size_t _requestedBytes = 0;
        // Content version -> this frame's offset (nothing for one that did not fit).
        std::unordered_map<uint64_t, std::optional<size_t>> _shared;
    };
}
