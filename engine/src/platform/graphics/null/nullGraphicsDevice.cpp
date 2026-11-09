// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
#include "nullGraphicsDevice.h"

#include <cstring>

#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/vertexBuffer.h"

namespace visutwin::canvas
{
    namespace
    {
        // Keeps its bytes (VertexBuffer's storage) and uploads nothing.
        class NullVertexBuffer final : public VertexBuffer
        {
        public:
            using VertexBuffer::VertexBuffer;
            void unlock() override {}
        };

        class NullIndexBuffer final : public IndexBuffer
        {
        public:
            using IndexBuffer::IndexBuffer;

            bool setData(const std::vector<uint8_t>& data) override
            {
                _storage = data;
                return true;
            }

            bool writeRange(const size_t offset, const void* data, const size_t size) override
            {
                if (offset + size > _storage.size()) {
                    return false;
                }
                std::memcpy(_storage.data() + offset, data, size);
                return true;
            }
        };

        class NullRenderTarget final : public RenderTarget
        {
        public:
            using RenderTarget::RenderTarget;

        protected:
            void destroyFrameBuffers() override {}
            void createFrameBuffers() override {}
        };
    }

    NullGraphicsDevice::NullGraphicsDevice(const int width, const int height) : _size(width, height) {}

    void NullGraphicsDevice::draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>&,
        const int numInstances, int, bool, bool)
    {
        recordDraw(primitive, numInstances);
    }

    void NullGraphicsDevice::startRenderPass(RenderPass*) { _insideRenderPass = true; }

    void NullGraphicsDevice::endRenderPass(RenderPass*) { _insideRenderPass = false; }

    std::unique_ptr<gpu::HardwareTexture> NullGraphicsDevice::createGPUTexture(Texture*) { return nullptr; }

    std::shared_ptr<VertexBuffer> NullGraphicsDevice::createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
        const int numVertices, const VertexBufferOptions& options)
    {
        return std::make_shared<NullVertexBuffer>(this, format, numVertices, options);
    }

    std::shared_ptr<IndexBuffer> NullGraphicsDevice::createIndexBuffer(const IndexFormat format, const int numIndices,
        const std::vector<uint8_t>& data)
    {
        auto buffer = std::make_shared<NullIndexBuffer>(this, format, numIndices);
        buffer->setData(data);
        return buffer;
    }

    std::shared_ptr<RenderTarget> NullGraphicsDevice::createRenderTarget(const RenderTargetOptions& options)
    {
        RenderTargetOptions withDevice = options;
        withDevice.graphicsDevice = this;
        return std::make_shared<NullRenderTarget>(withDevice);
    }

    void NullGraphicsDevice::setResolution(const int width, const int height) { _size = {width, height}; }
}
