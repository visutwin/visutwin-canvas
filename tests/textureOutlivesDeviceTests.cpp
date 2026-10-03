// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 03.10.2026
//
// A Texture may outlive the GraphicsDevice it was made on: an asset held by a global is
// destroyed after main returns, long after the device. The device detaches every live
// texture as it is torn down, releasing its GPU texture while the device can still free
// it, and the texture forgets the device. Before that, the texture's destructor wrote the
// freed device's VRAM counters (a crash at exit, or silent heap damage). Under the
// sanitize preset any touch of the freed device here aborts.

#include <iostream>
#include <memory>
#include <vector>

#include "platform/graphics/gpu.h"
#include "platform/graphics/texture.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    bool deviceAlive = false;
    int gpuTexturesReleased = 0;
    int gpuTexturesReleasedWithoutDevice = 0;

    // A GPU texture that records whether its device still existed when it was released.
    class RecordingGpuTexture final : public gpu::HardwareTexture
    {
    public:
        ~RecordingGpuTexture() override
        {
            ++gpuTexturesReleased;
            if (!deviceAlive) {
                ++gpuTexturesReleasedWithoutDevice;
            }
        }
        void uploadImmediate(GraphicsDevice*) override {}
        void propertyChanged(uint32_t) override {}
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

        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override
        {
            return std::make_unique<RecordingGpuTexture>();
        }
    };

    TextureOptions options()
    {
        TextureOptions o;
        o.name = "outlives";
        o.width = 64;
        o.height = 32;
        o.format = PixelFormat::PIXELFORMAT_RGBA8;
        o.mipmaps = false;
        return o;
    }
}

int main()
{
    std::cout << "a texture destroyed before its device\n";
    {
        auto device = std::make_unique<RecordingDevice>();
        {
            Texture texture(device.get(), options());
            check(device->vram().tex > 0, "the texture is counted in the device's VRAM");
            check(texture.device() == device.get() && texture.impl() != nullptr, "and has its device and GPU texture");
        }
        check(device->vram().tex == 0, "destroying it gives its VRAM share back");
        check(gpuTexturesReleased == 1 && gpuTexturesReleasedWithoutDevice == 0,
            "its GPU texture is released with it, the device alive");
    }

    std::cout << "\na texture that outlives its device\n";
    {
        gpuTexturesReleased = 0;
        auto device = std::make_unique<RecordingDevice>();
        auto survivor = std::make_unique<Texture>(device.get(), options());
        auto second = std::make_unique<Texture>(device.get(), options());
        // One more the device owns, which teardown destroys outright.
        device->addTexture(std::make_shared<Texture>(device.get(), options()));

        device.reset();

        check(gpuTexturesReleased == 3, "teardown released all three GPU textures");
        check(gpuTexturesReleasedWithoutDevice == 0, "each while the device could still free it");
        check(survivor->device() == nullptr && survivor->impl() == nullptr,
            "a surviving texture forgets the device and holds no GPU texture");

        // Everything that would reach the device is now a quiet no-op.
        survivor->upload();
        survivor->resize(128, 64);
        std::vector<uint8_t> pixels;
        check(!survivor->read(pixels, 0, 0, 1, 1), "reading it back fails cleanly");
        check(survivor->width() == 128 && survivor->height() == 64, "its CPU-side state still works");

        // The crash this guards: destroying it touched the freed device.
        survivor.reset();
        second.reset();
        check(gpuTexturesReleased == 3, "destroying the survivors releases nothing twice");
    }

    std::cout << "\na texture made on a device that has no GPU textures\n";
    {
        auto device = std::make_unique<StubGraphicsDevice>();
        auto survivor = std::make_unique<Texture>(device.get(), options());
        // The stub device does not call releaseGpuReferences: its base destructor detaches.
        device.reset();
        check(survivor->device() == nullptr, "the base destructor detaches it too");
        survivor.reset();
    }

    return finish("texture outlives device");
}
