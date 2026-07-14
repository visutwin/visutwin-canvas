// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <core/math/matrix4.h>
#include <core/math/vector3.h>

#include "gsplatSorter.h"

namespace visutwin::canvas
{
    class GSplatResource;
    class VertexBuffer;

    /** Mirrors the MSL GSplatParams struct at vertex buffer slot 11 (160 bytes). */
    struct GpuGSplatParams
    {
        Matrix4 modelView;
        Matrix4 projection;
        float viewport[4];      // width, height, 1/width, 1/height
        uint32_t splatCount;
        uint32_t shBands;       // 0 = SH0 only; 1-3 evaluate view-dependent color
        uint32_t pad[2];
    };
    static_assert(sizeof(GpuGSplatParams) == 160);

    /**
     * Per-mesh-instance Gaussian splat state: the background depth sorter and the
     * ping-pong order buffers it fills (uint32 splat index per instance, farthest
     * first). The renderer calls update() each frame before drawing.
     */
    class GSplatInstance
    {
    public:
        explicit GSplatInstance(const std::shared_ptr<GSplatResource>& resource);

        GSplatResource* resource() const { return _resource.get(); }

        /**
         * Kick/poll the sorter with this frame's camera and build the GPU params.
         * cameraPosition/cameraForward are world-space; model is the splat node's
         * world transform (the sort runs in splat model space).
         */
        void update(const Vector3& cameraPosition, const Vector3& cameraForward,
                    const Matrix4& model, const Matrix4& view, const Matrix4& projection,
                    float viewportWidth, float viewportHeight);

        const std::shared_ptr<VertexBuffer>& orderBuffer() const { return _orderBuffers[_activeOrderBuffer]; }
        uint32_t visibleCount() const { return _visibleCount; }
        const GpuGSplatParams& gpuParams() const { return _gpuParams; }

    private:
        std::shared_ptr<GSplatResource> _resource;
        std::unique_ptr<GSplatSorter> _sorter;

        // Ping-pong order buffers: the sorter result uploads into the inactive one
        // to avoid stomping data a queued frame may still read.
        std::shared_ptr<VertexBuffer> _orderBuffers[2];
        int _activeOrderBuffer = 0;
        uint32_t _visibleCount = 0;

        std::vector<uint32_t> _fetchScratch;
        GpuGSplatParams _gpuParams{};
    };
}
