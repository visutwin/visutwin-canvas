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
        // Output stage (upstream gsplatOutput's prepareOutputFromGamma). A splat's
        // colour is GAMMA space; these say what the target wants done to it. Filled
        // by the renderer per draw from the same scene state the forward pass reads.
        float fogColor[4];      // linear rgb, unused
        float fogParams[4];     // start, end, density, type (FogType; 0 = none)
        float output[4];        // exposure, tone mapping mode, linear HDR target (0/1), unused
    };
    static_assert(sizeof(GpuGSplatParams) == 208);

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

        /**
         * The model-space direction the sorter projects splat centres onto, so that
         * dot(localCentre, result) orders splats by their true world-space depth
         * along cameraForward. Each local axis is weighted by the model matrix's own
         * basis vector projected on the view direction, which is exact for any affine
         * transform; the result is normalised, which is order-preserving.
         *
         * Public and static so it can be tested without a device or a loaded splat.
         */
        static Vector3 sortDirection(const Matrix4& model, const Vector3& cameraForward);

        const std::shared_ptr<VertexBuffer>& orderBuffer() const { return _orderBuffers[_activeOrderBuffer]; }
        uint32_t visibleCount() const { return _visibleCount; }
        const GpuGSplatParams& gpuParams() const { return _gpuParams; }

    private:
        std::shared_ptr<GSplatResource> _resource;
        std::unique_ptr<GSplatSorter> _sorter;

        // One order buffer per frame the CPU may run ahead of the GPU
        // (GraphicsDevice::maxFramesInFlight), cycled on each adopted sort.
        //
        // Two is NOT enough: Metal writes a shared-storage buffer in place while
        // keeping three frames in flight, so a result landing on two consecutive
        // frames comes back round to a buffer the frame before last may still be
        // reading, and the splats tear mid-draw. Upstream has no such rule because
        // its upload is queue-ordered, which is also why Vulkan cannot tear here.
        std::vector<std::shared_ptr<VertexBuffer>> _orderBuffers;
        size_t _activeOrderBuffer = 0;
        uint32_t _visibleCount = 0;

        // Render version of the frame that last adopted a sort. One upload per
        // frame is what makes the cycle above long enough: a splat drawn by
        // several cameras in one frame must not walk the cycle several times.
        int _lastUploadVersion = -1;

        std::vector<uint32_t> _fetchScratch;
        GpuGSplatParams _gpuParams{};
    };
}
