// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.10.2025.
//
#pragma once

#include "metalBuffer.h"
#include "platform/graphics/vertexBuffer.h"

namespace visutwin::canvas
{
    /**
     * A Metal implementation of the VertexBuffer.
     */
    class MetalVertexBuffer : public VertexBuffer, gpu::MetalBuffer
    {
    public:
        MetalVertexBuffer(GraphicsDevice* graphicsDevice, const std::shared_ptr<VertexFormat>& format, int numVertices,
            const VertexBufferOptions& options = VertexBufferOptions{});

        /// Zero-copy constructor: adopts an externally-created MTL::Buffer.
        /// Used for GPU compute output (e.g., Marching Cubes) where the buffer
        /// is already filled by the GPU and no CPU-side copy is needed.
        MetalVertexBuffer(GraphicsDevice* device, const std::shared_ptr<VertexFormat>& format,
            int numVertices, MTL::Buffer* externalBuffer);

        void unlock() override;

        [[nodiscard]] MTL::Buffer* raw() const { return gpu::MetalBuffer::raw(); }

        void* nativeBuffer() const override { return raw(); }
    };
}
