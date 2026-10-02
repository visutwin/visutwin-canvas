// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The unit tests' GraphicsDevice: no GPU, configured by what a test needs.
//
// By default every buffer, texture and render target it is asked for is NULL and its size
// is fixed at {0, 0}; a test that exercises the null path keeps that. Options turn on
// what a test needs instead:
//
//   size              what size() answers (and what setResolution starts from)
//   resizable         setResolution stores the new size (UI tests: the canvas follows
//                     it); otherwise setResolution does nothing
//   cpuBuffers        vertex and index buffers are CPU-backed (CpuVertexBuffer and
//                     CpuIndexBuffer), so a mesh keeps its bytes and VRAM is counted
//   keepIndexData     the index buffers keep their bytes and accept writeRange
//                     (KeepingIndexBuffer); needs cpuBuffers
//   nullIndexBuffers  with cpuBuffers, only the VERTEX buffers are CPU-backed
//   recordDraws       draw() records the draw into the frame counters and a pass the
//                     device begins is marked inside (what a backend does for the
//                     statistics)
//   renderTargets     createRenderTarget builds a StubRenderTarget on this device
//
// A test that needs another override (a counter, a capability) derives from it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/vertexBuffer.h"

namespace visutwin::canvas::test
{
    /// A vertex buffer that keeps its bytes on the CPU and uploads nothing.
    class CpuVertexBuffer : public VertexBuffer
    {
    public:
        using VertexBuffer::VertexBuffer;

        /// The zero-copy constructor, public here: an empty buffer for an empty mesh.
        CpuVertexBuffer(GraphicsDevice* device, std::shared_ptr<VertexFormat> format, const int numVertices,
            const int numBytes)
            : VertexBuffer(device, std::move(format), numVertices, numBytes)
        {
        }

        void unlock() override {}
    };

    /// An index buffer that accepts its data and keeps none of it.
    class CpuIndexBuffer final : public IndexBuffer
    {
    public:
        using IndexBuffer::IndexBuffer;
        bool setData(const std::vector<uint8_t>&) override { return true; }
    };

    /// An index buffer that keeps its data and can overwrite a range of it.
    class KeepingIndexBuffer final : public IndexBuffer
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

    /// A render target with no frame buffers behind it.
    class StubRenderTarget final : public RenderTarget
    {
    public:
        using RenderTarget::RenderTarget;
    protected:
        void destroyFrameBuffers() override {}
        void createFrameBuffers() override {}
    };

    struct StubDeviceOptions
    {
        std::pair<int, int> size{0, 0};
        bool resizable = false;
        bool cpuBuffers = false;
        bool keepIndexData = false;
        bool nullIndexBuffers = false;
        bool recordDraws = false;
        bool renderTargets = false;
    };

    class StubGraphicsDevice : public GraphicsDevice
    {
    public:
        using Options = StubDeviceOptions;

        explicit StubGraphicsDevice(const Options& options = {}) : _options(options), _size(options.size) {}

        void draw(const Primitive& primitive, const std::shared_ptr<IndexBuffer>&, const int numInstances, int,
            bool, bool) override
        {
            if (_options.recordDraws) {
                recordDraw(primitive, numInstances);
            }
        }

        // A pass the device agrees to begin is one whose execute() runs.
        void startRenderPass(RenderPass*) override
        {
            if (_options.recordDraws) {
                _insideRenderPass = true;
            }
        }

        void endRenderPass(RenderPass*) override
        {
            if (_options.recordDraws) {
                _insideRenderPass = false;
            }
        }

        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }

        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>& format,
            const int numVertices, const VertexBufferOptions& options) override
        {
            if (!_options.cpuBuffers) {
                return nullptr;
            }
            return std::make_shared<CpuVertexBuffer>(this, format, numVertices, options);
        }

        std::shared_ptr<IndexBuffer> createIndexBuffer(const IndexFormat format, const int numIndices,
            const std::vector<uint8_t>& data) override
        {
            if (!_options.cpuBuffers || _options.nullIndexBuffers) {
                return nullptr;
            }
            std::shared_ptr<IndexBuffer> buffer;
            if (_options.keepIndexData) {
                buffer = std::make_shared<KeepingIndexBuffer>(this, format, numIndices);
            } else {
                buffer = std::make_shared<CpuIndexBuffer>(this, format, numIndices);
            }
            buffer->setData(data);
            return buffer;
        }

        void setResolution(const int width, const int height) override
        {
            if (_options.resizable) {
                _size = {width, height};
            }
        }

        std::pair<int, int> size() const override { return _size; }

        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions& options) override
        {
            if (!_options.renderTargets) {
                return nullptr;
            }
            RenderTargetOptions withDevice = options;
            withDevice.graphicsDevice = this;
            return std::make_shared<StubRenderTarget>(withDevice);
        }

    private:
        Options _options;
        std::pair<int, int> _size;
    };
}
