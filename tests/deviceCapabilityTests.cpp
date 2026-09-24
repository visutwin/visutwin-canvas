// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The device capability queries, and the two decisions that key on them.
//
// Nothing in the tree can show either one. Every GPU this engine runs on renders
// half-float colour attachments and allocates textures far past the 4096 that five
// call sites used to spell as a literal, so the fallback path and the clamp path are
// both unreachable from an example — a render proves only that the capable case
// still works. A stub device that ANSWERS differently is the only way to exercise
// what happens when a device says no.
//
// What is pinned here:
//   - a VSM_16F request on a device without half-float render targets resolves to
//     PCF3 (upstream's documented fallback), while the REQUEST is remembered;
//   - that resolution survives LightComponent::syncToLight's per-frame replay of
//     the same value without dropping the shadow map each frame — the trap every
//     shadow-map-invalidating setter shares;
//   - the shadow resolution is clamped to maxTextureSize, and to maxCubeMapSize
//     for an omni, which are different limits on real hardware.

#include <iostream>
#include <memory>

#include "platform/graphics/graphicsDevice.h"
#include "scene/light.h"
#include "scene/renderer/shadowMap.h"

using namespace visutwin::canvas;

namespace
{
    /// A device that creates nothing and only answers capability questions. The
    /// nine pure virtuals below are the whole GPU-facing surface; none is reached.
    class StubDevice final : public GraphicsDevice
    {
    public:
        /// Answers nothing, so every capability reads its base-class default.
        StubDevice() = default;

        StubDevice(const int maxTexture, const int maxCube, const bool halfFloatRenderable)
        {
            setMaxTextureSize(maxTexture);
            setMaxCubeMapSize(maxCube);
            setTextureHalfFloatRenderable(halfFloatRenderable);
        }

        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool,
            bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override
        {
            return nullptr;
        }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&,
            int, const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int,
            const std::vector<uint8_t>&) override { return nullptr; }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override
        {
            return nullptr;
        }
    };

    void giveShadowMap(Light& light)
    {
        light.setShadowMap(ShadowMap::createAtlas(nullptr, nullptr));
    }

    bool checkDefaults()
    {
        // A backend that fills in nothing must behave as the literals it replaced:
        // 4096 either way, and no float render targets, so an unanswered capability
        // loses a feature rather than allocating a target the driver would refuse.
        StubDevice bare;
        const GraphicsDevice* device = &bare;
        if (device->maxTextureSize() != 4096 || device->maxCubeMapSize() != 4096) {
            std::cerr << "the dimension defaults no longer match the 4096 literals they"
                         " replaced, so an unanswering backend changed behaviour\n";
            return false;
        }
        if (device->textureFloatRenderable()) {
            std::cerr << "textureFloatRenderable defaults to true, so a backend that never"
                         " answers claims a capability it was never asked about\n";
            return false;
        }
        if (device->supportsTimestampQuery()) {
            std::cerr << "supportsTimestampQuery is true without a profiler\n";
            return false;
        }
        return true;
    }

    bool checkVsmFallback()
    {
        StubDevice noHalfFloat(16384, 16384, false);
        Light light(&noHalfFloat);

        light.setShadowType(SHADOW_VSM_16F);
        if (light.shadowType() != SHADOW_PCF3_32F) {
            std::cerr << "VSM_16F was kept on a device with no half-float render target;"
                         " the shadow pass has nowhere to write its moments\n";
            return false;
        }
        if (light.requestedShadowType() != SHADOW_VSM_16F) {
            std::cerr << "the fallback overwrote what the caller asked for\n";
            return false;
        }

        // The trap: LightComponent::syncToLight pushes the same request onto the
        // Light every frame. Comparing it against the RESOLVED type would differ
        // forever and drop the shadow map on each replay.
        giveShadowMap(light);
        light.setShadowType(SHADOW_VSM_16F);
        if (light.shadowMap() == nullptr) {
            std::cerr << "replaying the same VSM_16F request dropped the shadow map, so"
                         " the per-frame sync would reallocate it forever\n";
            return false;
        }

        // A device that CAN render the moments keeps the type it was given.
        StubDevice capable(16384, 16384, true);
        Light capableLight(&capable);
        capableLight.setShadowType(SHADOW_VSM_16F);
        if (capableLight.shadowType() != SHADOW_VSM_16F) {
            std::cerr << "VSM_16F fell back on a device that supports it\n";
            return false;
        }
        return true;
    }

    bool checkResolutionClamp()
    {
        // maxImageDimension2D and maxImageDimensionCube are separate Vulkan limits
        // and are not always equal, so an omni is clamped against its own.
        StubDevice device(8192, 2048, true);

        Light spot(&device);
        spot.setType(LightType::LIGHTTYPE_SPOT);
        spot.setShadowResolution(16384);
        if (spot.shadowResolution() != 8192) {
            std::cerr << "the shadow resolution was not clamped to maxTextureSize (got "
                      << spot.shadowResolution() << ")\n";
            return false;
        }

        Light omni(&device);
        omni.setType(LightType::LIGHTTYPE_OMNI);
        omni.setShadowResolution(4096);
        if (omni.shadowResolution() != 2048) {
            std::cerr << "an omni's cube shadow map was clamped against the 2D limit"
                         " instead of maxCubeMapSize (got " << omni.shadowResolution()
                      << ")\n";
            return false;
        }

        // Clamped to the same value twice must still be a no-op: the clamp happens
        // before the unchanged-value guard, so two different requests that clamp
        // equal must not drop the map a second time.
        giveShadowMap(omni);
        omni.setShadowResolution(8192);
        if (omni.shadowMap() == nullptr) {
            std::cerr << "two oversized requests that clamp to the same resolution"
                         " dropped the shadow map twice\n";
            return false;
        }
        return true;
    }
}

int main()
{
    const bool ok = checkDefaults() && checkVsmFallback() && checkResolutionClamp();
    if (!ok) {
        std::cerr << "device capability tests FAILED\n";
        return 1;
    }
    std::cout << "device capability tests passed\n";
    return 0;
}
