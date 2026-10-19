// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.07.2026
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
        uint32_t shBands;       // 0 = SH0 only; 1-4 evaluate view-dependent color
        // 1 for an orthographic camera: its view rays all run along the camera forward,
        // so SH is evaluated along that, not from the camera position.
        uint32_t cameraOrtho;
        uint32_t pad;
        // Output stage. A splat's
        // colour is GAMMA space; these say what the target wants done to it. Filled
        // by the renderer per draw from the same scene state the forward pass reads.
        float fogColor[4];      // linear rgb, unused
        float fogParams[4];     // start, end, density, type (FogType; 0 = none)
        float output[4];        // exposure, tone mapping mode, linear HDR target (0/1), unused;
                                // a shadow draw's: alpha clip, VSM mode, far clip, unused
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

        /**
         * The parameter block of a SHADOW draw (the gsplat-render "shadow" variants), which
         * draws every splat in index order from a shadow camera into a target of the given
         * size. `vsmMode` 0 writes depth only, 1 EVSM moments of the depth, 2 of the
         * distance from the light over `farClip` (a perspective pass).
         */
        GpuGSplatParams shadowParams(const Matrix4& model, const Matrix4& view, const Matrix4& projection,
            float viewportWidth, float viewportHeight, bool orthographic, int vsmMode, float farClip) const;

        /// A shadow fragment whose gaussian alpha is below this is not a caster (default 0.3).
        float alphaClip() const { return _alphaClip; }
        void setAlphaClip(const float value) { _alphaClip = value; }

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
        // reading, and the splats tear mid-draw. Vulkan cannot tear here, because
        // its upload is queue-ordered.
        std::vector<std::shared_ptr<VertexBuffer>> _orderBuffers;
        size_t _activeOrderBuffer = 0;
        uint32_t _visibleCount = 0;

        // Render version of the frame that last adopted a sort. One upload per
        // frame is what makes the cycle above long enough: a splat drawn by
        // several cameras in one frame must not walk the cycle several times.
        int _lastUploadVersion = -1;

        std::vector<uint32_t> _fetchScratch;
        GpuGSplatParams _gpuParams{};
        float _alphaClip = 0.3f;
    };
}
