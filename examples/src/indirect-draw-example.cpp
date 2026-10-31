// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream compute/indirect-draw.
//
// A compute shader writes the arguments of an indirect draw every frame. A sphere is
// hardware-instanced 1,000 times (a fifth of its size, scattered through a 5-unit cube), and the
// kernel sets how many instances the draw takes, |sin(t)| of them, together with the sphere's
// index count, first index and base vertex. The draw arguments go into a slot of the device's
// indirect draw buffer, taken every frame, which the mesh instance draws from
// (MeshInstance::setIndirect). The spheres are a pale yellow metal (gloss 1) under the helipad
// environment (skybox mip 2, exposure 0.7); the camera orbits the origin at 8 units, 0.2 radians a
// second, and tone-maps ACES. The performance HUD shows the frame's compute dispatches and passes.
//
// DEVIATIONS:
// - The kernel is Slang: the draw arguments at buffer index 0 and the uniform block after it
//   (compute.h), the arguments written as five uints a slot.
// - The instances are placed by a fixed-seed generator, so every run scatters them alike.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kInstanceCount = 1000;

    // Writes one indexed draw's arguments: the mesh's index range and an oscillating instance count.
    constexpr const char* kIndirectDrawSlang = R"slang(
struct IndirectDrawParams
{
    int4 indirectMetaData;  // x: index count, y: first index, z: base vertex
    float4 time;            // x: time in seconds
    uint4 slots;            // x: the maximum instance count, y: the slot in the draw buffer
};

// Five uints a slot: index count, instance count, first index, base vertex, first instance
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> indirectDrawBuffer : register(u0);
[[vk::binding(1, 0)]] ConstantBuffer<IndirectDrawParams> params : register(b1);

[shader("compute")]
[numthreads(1, 1, 1)]
void indirectDrawMain(uint3 id : SV_DispatchThreadID)
{
    const int4 metaData = params.indirectMetaData;

    // Generate oscillating instance count using a sine wave
    const float wave = abs(sin(params.time.x));
    const uint visibleCount = uint(wave * float(params.slots.x));

    // generate draw call parameters based on metadata. Supply computed number of instances.
    const uint base = params.slots.y * 5u;
    indirectDrawBuffer[base + 0u] = uint(metaData.x);
    indirectDrawBuffer[base + 1u] = visibleCount;
    indirectDrawBuffer[base + 2u] = uint(metaData.y);
    indirectDrawBuffer[base + 3u] = asuint(metaData.z);
    indirectDrawBuffer[base + 4u] = 0u;
}
)slang";

    struct IndirectDrawParams
    {
        int32_t indirectMetaData[4];
        float time[4];
        uint32_t slots[4];
    };
}

class IndirectDrawExample final : public ExampleApp
{
public:
    IndirectDrawExample() : ExampleApp({.title = "Compute Indirect Draw", .miniStatsPresets = {"compute"}}) {}

protected:
    bool create() override
    {
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* helipad = _helipadAsset->resourceAs<Texture>();
        if (!helipad) {
            spdlog::error("indirect-draw needs the helipad env atlas");
            return false;
        }

        // Setup skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(0.7f);
        scene()->setEnvAtlas(helipad);

        _camera = createCamera(Vector3(0.0f, 0.0f, 10.0f));
        _camera->findComponent<CameraComponent>()->setToneMapping(TONEMAP_ACES);

        // The material of the instanced spheres
        _material = std::make_shared<StandardMaterial>();
        _material->setDiffuse(Color(1.0f, 1.0f, 0.5f, 1.0f));
        _material->setGloss(1.0f);
        _material->setMetalness(1.0f);
        _material->setUseMetalness(true);

        Entity* sphere = createPrimitive("sphere", _material.get());
        sphere->setName("InstancingEntity");
        auto* render = sphere->findComponent<RenderComponent>();
        if (!render || render->meshInstances().empty()) {
            spdlog::error("indirect-draw: the sphere has no mesh instance");
            return false;
        }
        _meshInstance = render->meshInstances()[0];

        // The instance matrices: random positions in a 5-unit cube, a fifth of the size
        constexpr float radius = 5.0f;
        std::mt19937 random(42u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        constexpr int kInstanceBytes = VertexFormat::INSTANCING_MATRIX_SIZE;
        std::vector<uint8_t> instanceBytes(static_cast<size_t>(kInstanceCount) * kInstanceBytes);
        for (int i = 0; i < kInstanceCount; ++i) {
            const Vector3 position(unit(random) * radius - radius * 0.5f, unit(random) * radius - radius * 0.5f,
                unit(random) * radius - radius * 0.5f);
            const Matrix4 matrix = Matrix4::trs(position, Quaternion::fromEulerAngles(0.0f, 0.0f, 0.0f),
                Vector3(0.2f, 0.2f, 0.2f));
            std::memcpy(instanceBytes.data() + static_cast<size_t>(i) * kInstanceBytes, &matrix, kInstanceBytes);
        }
        VertexBufferOptions options;
        options.data = std::move(instanceBytes);
        _instances = device()->createVertexBuffer(VertexFormat::defaultInstancingFormat(), kInstanceCount, options);
        _meshInstance->setInstancing(_instances, kInstanceCount);

        if (!device()->supportsCompute()) {
            spdlog::warn("indirect-draw: this device has no compute; every instance is drawn");
            return true;
        }
        _shader = getOrCreateSlangShaderFromSource(device().get(), "indirect-draw-compute", kIndirectDrawSlang);
        if (!_shader) {
            spdlog::error("indirect-draw: the compute shader did not compile");
            return false;
        }

        // The compute shader, with the values that do not change each frame
        _compute = std::make_unique<Compute>(device().get(), _shader, "ComputeIndirectDraw");
        const auto metaData = _meshInstance->getIndirectMetaData();
        std::copy(metaData.begin(), metaData.end(), _params.indirectMetaData);
        _params.slots[0] = kInstanceCount;
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        if (_compute) {
            // A slot in the indirect draw buffer, taken again every frame, which the mesh instance
            // draws from for every camera (null)
            const int slot = device()->getIndirectDrawSlot();
            if (slot >= 0) {
                _meshInstance->setIndirect(nullptr, slot);

                // The kernel writes the slot's arguments
                _compute->setParameter("indirectDrawBuffer", device()->indirectDrawBuffer());
                _params.time[0] = _time;
                _params.slots[1] = static_cast<uint32_t>(slot);
                _compute->setUniformBlock(&_params, sizeof(_params));
                _compute->setupDispatch(1u, 1u, 1u);
                device()->computeDispatch({_compute.get()}, "ComputeIndirectDraw");
            }
        }

        // Orbit camera around
        _angle += dt * 0.2f;
        _camera->setLocalPosition(8.0f * std::sin(_angle), 0.0f, 8.0f * std::cos(_angle));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
    }

private:
    std::unique_ptr<Asset> _helipadAsset;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<VertexBuffer> _instances;
    std::shared_ptr<Shader> _shader;
    std::unique_ptr<Compute> _compute;
    Entity* _camera = nullptr;
    MeshInstance* _meshInstance = nullptr;
    IndirectDrawParams _params{};
    float _time = 0.0f;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(IndirectDrawExample)
