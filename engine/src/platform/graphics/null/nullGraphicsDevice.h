// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// A graphics device with no GPU behind it: the whole engine runs (scene graph, components,
// culling, the frame graph, the draw statistics) and nothing is drawn. Vertex and index
// buffers keep their bytes on the CPU, so meshes have bounds and VRAM is counted; textures have
// no GPU object; render targets have no frame buffers. For logic run without a window and for
// tests on machines without a GPU (the C API's headless engine, VT_BACKEND_NULL).
//
#pragma once

#include <memory>
#include <utility>

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class NullGraphicsDevice final : public GraphicsDevice
    {
    public:
        /// A back buffer of `width` x `height`, which setResolution changes.
        NullGraphicsDevice(int width, int height);

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>& indexBuffer, int numInstances,
            int indirectSlot, bool first, bool last) override;
        void startRenderPass(RenderPass* renderPass) override;
        void endRenderPass(RenderPass* renderPass) override;

        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture* texture) override;
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            int numVertices, const VertexBufferOptions& options) override;
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat format, int numIndices,
            const std::vector<uint8_t>& data) override;
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions& options) override;

        void setResolution(int width, int height) override;
        std::pair<int, int> size() const override { return _size; }

        /// It draws nothing by design, so a missing feature is not worth a warning.
        bool reportsUnsupportedFeatures() const override { return false; }

    private:
        std::pair<int, int> _size;
    };
}
