// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// Render targets, vertex buffers and index buffers may outlive the GraphicsDevice they
// were made on, as textures may (textureOutlivesDeviceTests.cpp): a mesh or a pass held by
// a global is destroyed after main returns, long after the device. The device detaches each
// one as it is torn down, releasing its GPU objects while the device can still free them,
// and the resource forgets the device. Before that, the destructors wrote the freed
// device's VRAM counters and render-target registry, and a backend target asked the freed
// device for its native handles. Under the sanitize preset any touch of the freed device
// here aborts.

#include <iostream>
#include <memory>
#include <vector>

#include "platform/graphics/gpu.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    bool deviceAlive = false;

    // How many GPU objects each kind released, and how many of those after the device
    // was gone (which on a real backend frees into a destroyed allocator).
    struct Releases
    {
        int total = 0;
        int withoutDevice = 0;

        void record()
        {
            ++total;
            if (!deviceAlive) {
                ++withoutDevice;
            }
        }
    };

    Releases vertexReleases;
    Releases indexReleases;
    Releases targetReleases;
    int vertexUploads = 0;

    class RecordingVertexBuffer final : public VertexBuffer
    {
    public:
        using VertexBuffer::VertexBuffer;
        ~RecordingVertexBuffer() override
        {
            // A buffer the device detached has released its GPU buffer already.
            if (!_released) {
                vertexReleases.record();
            }
        }
        void unlock() override { ++vertexUploads; }

    protected:
        void releaseGpuBuffer() override
        {
            vertexReleases.record();
            _released = true;
        }

    private:
        bool _released = false;
    };

    class RecordingIndexBuffer final : public IndexBuffer
    {
    public:
        using IndexBuffer::IndexBuffer;
        ~RecordingIndexBuffer() override
        {
            if (!_released) {
                indexReleases.record();
            }
        }
        bool setData(const std::vector<uint8_t>& data) override
        {
            // As the backends: nothing to upload into once the GPU buffer is gone.
            if (_released) {
                return false;
            }
            _storage = data;
            return true;
        }

    protected:
        void releaseGpuBuffer() override
        {
            indexReleases.record();
            _released = true;
        }

    private:
        bool _released = false;
    };

    class RecordingRenderTarget final : public RenderTarget
    {
    public:
        explicit RecordingRenderTarget(const RenderTargetOptions& options) : RenderTarget(options)
        {
            createFrameBuffers();
        }
        // As the backends: the destructor destroys whatever frame buffers are left.
        ~RecordingRenderTarget() override { destroyFrameBuffers(); }

        bool hasFrameBuffers() const { return _hasFrameBuffers; }

    protected:
        void destroyFrameBuffers() override
        {
            if (_hasFrameBuffers) {
                targetReleases.record();
                _hasFrameBuffers = false;
            }
        }
        void createFrameBuffers() override { _hasFrameBuffers = true; }

    private:
        bool _hasFrameBuffers = false;
    };

    // Tears down as the real backends do: base-owned GPU objects first, then its own state.
    class RecordingDevice final : public StubGraphicsDevice
    {
    public:
        RecordingDevice() { deviceAlive = true; }
        ~RecordingDevice() override
        {
            releaseGpuReferences();
            deviceAlive = false;
        }

        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
    };

    std::shared_ptr<VertexFormat> format()
    {
        return std::make_shared<VertexFormat>(56, VertexFormat::standardElements(), true, false);
    }

    std::shared_ptr<Texture> colorTexture(GraphicsDevice* device)
    {
        TextureOptions o;
        o.name = "outlives-target";
        o.width = 64;
        o.height = 32;
        o.format = PixelFormat::PIXELFORMAT_RGBA8;
        o.mipmaps = false;
        return std::make_shared<Texture>(device, o);
    }

    std::unique_ptr<RecordingRenderTarget> target(GraphicsDevice* device, Texture* color)
    {
        RenderTargetOptions o;
        o.graphicsDevice = device;
        o.colorBuffer = color;
        o.name = "outlives";
        return std::make_unique<RecordingRenderTarget>(o);
    }

    void reset()
    {
        vertexReleases = {};
        indexReleases = {};
        targetReleases = {};
        vertexUploads = 0;
    }
}

int main()
{
    std::cout << "resources destroyed before their device\n";
    {
        reset();
        auto device = std::make_unique<RecordingDevice>();
        auto color = colorTexture(device.get());
        {
            RecordingVertexBuffer vertices(device.get(), format(), 4);
            RecordingIndexBuffer indices(device.get(), INDEXFORMAT_UINT16, 6);
            auto renderTarget = target(device.get(), color.get());
            check(device->vram().vb == 4 * 56 && device->vram().ib == 12, "the buffers are counted in the VRAM");
            check(renderTarget->hasFrameBuffers(), "the target has its frame buffers");
        }
        check(device->vram().vb == 0 && device->vram().ib == 0, "destroying them gives the VRAM back");
        check(vertexReleases.total == 1 && indexReleases.total == 1 && targetReleases.total == 1,
            "each released its GPU objects once");
        check(vertexReleases.withoutDevice + indexReleases.withoutDevice + targetReleases.withoutDevice == 0,
            "with the device alive");
    }

    std::cout << "\nresources that outlive their device\n";
    {
        reset();
        auto device = std::make_unique<RecordingDevice>();
        auto color = colorTexture(device.get());
        auto vertices = std::make_unique<RecordingVertexBuffer>(device.get(), format(), 4);
        auto storage = std::make_unique<RecordingVertexBuffer>(device.get(), format(), 2);
        storage->markStorageUse();
        auto indices = std::make_unique<RecordingIndexBuffer>(device.get(), INDEXFORMAT_UINT16, 6);
        auto renderTarget = target(device.get(), color.get());
        check(device->vram().sb == 2 * 56, "a storage buffer is counted as storage");

        device.reset();

        check(vertexReleases.total == 2 && indexReleases.total == 1 && targetReleases.total == 1,
            "teardown released every buffer and the target's frame buffers");
        check(vertexReleases.withoutDevice + indexReleases.withoutDevice + targetReleases.withoutDevice == 0,
            "each while the device could still free them");
        check(!renderTarget->hasFrameBuffers(), "the target holds no frame buffers");

        // Everything that would reach the device is now a quiet no-op, and the CPU-side
        // state still works.
        const std::vector<uint8_t> bytes(4 * 56, 7);
        check(vertices->setData(bytes) && vertexUploads == 0, "setting vertex data keeps the CPU copy, uploads nothing");
        check(vertices->storage()[0] == 7, "and the CPU copy holds it");
        const uint8_t one = 1;
        check(!vertices->writeRange(0, &one, 1), "writing a range is refused");
        vertices->markStorageUse();
        check(!indices->setData(std::vector<uint8_t>(12, 0)), "index data is refused");
        renderTarget->resize(128, 64);
        check(renderTarget->width() == 64 && renderTarget->height() == 32, "resizing the target does nothing");

        // The crash this guards: destroying them touched the freed device.
        vertices.reset();
        storage.reset();
        indices.reset();
        renderTarget.reset();
        color.reset();
        check(vertexReleases.total == 2 && indexReleases.total == 1 && targetReleases.total == 1,
            "destroying the survivors releases nothing twice");
    }

    std::cout << "\nresources on a device that never calls releaseGpuReferences\n";
    {
        reset();
        auto device = std::make_unique<StubGraphicsDevice>();
        auto vertices = std::make_unique<RecordingVertexBuffer>(device.get(), format(), 4);
        auto indices = std::make_unique<RecordingIndexBuffer>(device.get(), INDEXFORMAT_UINT16, 6);
        RenderTargetOptions o;
        o.graphicsDevice = device.get();
        o.depth = true;
        auto renderTarget = std::make_unique<RecordingRenderTarget>(o);
        // The stub device leaves it to its base destructor, which detaches them too.
        device.reset();
        check(vertexReleases.total == 1 && indexReleases.total == 1 && targetReleases.total == 1,
            "the base destructor detaches them");
        check(renderTarget->width() == 0, "a target sized by the device reads 0 once it is gone");
        vertices.reset();
        indices.reset();
        renderTarget.reset();
    }

    return finish("resources outlive device");
}
