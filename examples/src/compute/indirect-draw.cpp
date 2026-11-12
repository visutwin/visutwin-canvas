// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream compute/indirect-draw.
//
// A compute shader writes the arguments of an indirect draw every frame. A golden flower of beads:
// a sphere hardware-instanced along a golden spiral (each point turned by the golden angle from the
// last and a little further out, filling a disc evenly), keeping only the points inside the
// outline of five petals, curving up into a bowl, the beads growing toward the petal tips. The
// beads are stored from the centre out, so drawing the first N draws the flower up to a radius:
// the kernel sets that N, rising from none to all and falling back, easing in and out, with the
// sphere's index range, into a slot of the device's indirect draw buffer taken every frame, which
// the mesh instance draws from (MeshInstance::setIndirect). Polished gold under the product
// studio's environment, the camera rendering no skybox against a dark clear colour; a warm key
// light and two coloured rim lights. The camera frame (4x MSAA, neutral tone mapping) adds bloom
// and a vignette; the HUD shows the frame's compute dispatches and passes.
//
// Keys stand in for upstream's controls panel:
//   , / .        the speed -/+ 0.25 (0 - 3)
//   [ / ]        the most instances drawn -/+ 100 (0 - 2000)
//
// DEVIATIONS:
// - The kernel is Slang: the draw arguments at buffer index 0 and the uniform block after it
//   (compute.h), the arguments written as five uints a slot.
//
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

#include "exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
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

    // The instances drawn rise from none to all of them and fall back, easing in and out. They are
    // stored from the centre of the spiral out, so the flower grows and shrinks from its centre.
    const float wave = 0.5 - 0.5 * cos(params.time.x);
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
    IndirectDrawExample() : ExampleApp({.miniStatsPresets = {"compute"}}) {}

protected:
    bool create() override
    {
        _envAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/product-studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (Texture* env = _envAsset->resourceAs<Texture>()) {
            scene()->setEnvAtlas(env);
        }

        // Polished gold, on a sphere drawn instanced as the beads.
        _material = std::make_shared<StandardMaterial>();
        _material->setDiffuse(Color(1.0f, 0.76f, 0.45f, 1.0f));
        _material->setUseMetalness(true);
        _material->setMetalness(1.0f);
        _material->setGloss(0.8f);
        Entity* flower = createPrimitive("sphere", _material.get());
        flower->setName("Flower");
        auto* render = flower->findComponent<RenderComponent>();
        if (!render || render->meshInstances().empty()) {
            spdlog::error("indirect-draw: the sphere has no mesh instance");
            return false;
        }
        _meshInstance = render->meshInstances()[0];

        // The beads on a golden spiral inside the outline of five petals, from the centre out.
        const float goldenAngle = std::numbers::pi_v<float> * (3.0f - std::sqrt(5.0f));
        constexpr float flowerRadius = 5.0f;
        constexpr int points = 3200;
        constexpr int kInstanceBytes = VertexFormat::INSTANCING_MATRIX_SIZE;
        std::vector<uint8_t> instanceBytes;
        for (int i = 0; i < points; ++i) {
            const float distance = flowerRadius * std::sqrt(static_cast<float>(i) / points);
            const float angle = static_cast<float>(i) * goldenAngle;
            // The full radius at the middle of each petal, less than half of it between them.
            const float outline = flowerRadius * (0.35f + 0.65f * std::abs(std::cos(angle * 2.5f)));
            if (distance > outline) {
                continue;
            }
            const Vector3 position(distance * std::cos(angle), 0.1f * distance * distance, distance * std::sin(angle));
            const float size = 0.11f + 0.03f * distance;   // larger toward the petal tips
            const Matrix4 matrix = Matrix4::trs(position, Quaternion(0.0f, 0.0f, 0.0f, 1.0f), Vector3(size, size, size));
            const size_t offset = instanceBytes.size();
            instanceBytes.resize(offset + kInstanceBytes);
            std::memcpy(instanceBytes.data() + offset, &matrix, kInstanceBytes);
        }
        _instanceCount = static_cast<int>(instanceBytes.size() / kInstanceBytes);
        VertexBufferOptions options;
        options.data = std::move(instanceBytes);
        _instances = device()->createVertexBuffer(VertexFormat::defaultInstancingFormat(), _instanceCount, options);
        _meshInstance->setInstancing(_instances, _instanceCount);

        // The camera renders no skybox: the background is the clear colour (linear, as the
        // camera frame reads it).
        Entity* camera = createCamera(Vector3(0.0f, 8.5f, 12.0f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        Color clear;
        clear.fromString("#0c0e14");
        clear.linear();
        cameraComponent->camera()->setClearColor(clear);
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_IMMEDIATE, LAYERID_UI});
        cameraComponent->setToneMapping(TONEMAP_NEUTRAL);
        if (CameraControls* controls = addOrbitControls(camera, Vector3(0.0f, 1.2f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-89.0f, 0.0f));
            controls->setZoomRange(Vector2(4.0f, 20.0f));
            controls->storeResetState();
        }
        auto rendering = cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.02f;
        rendering.bloomBlurLevel = 6;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.5f;
        cameraComponent->setRendering(rendering);

        // A warm key light from the upper left and two coloured rim lights from behind.
        createLight("#fff1e0", 1.5f, Vector3(45.0f, -35.0f, 0.0f));
        createLight("#ff3da8", 2.0f, Vector3(-60.0f, 120.0f, 0.0f));
        createLight("#3d8bff", 2.0f, Vector3(-60.0f, -120.0f, 0.0f));

        if (!device()->supportsCompute()) {
            spdlog::warn("indirect-draw: this device has no compute; every instance is drawn");
            return true;
        }
        _shader = getOrCreateSlangShaderFromSource(device().get(), "indirect-draw-compute", kIndirectDrawSlang);
        if (!_shader) {
            spdlog::error("indirect-draw: the compute shader did not compile");
            return false;
        }

        // The compute shader, with the mesh's index range, which does not change.
        _compute = std::make_unique<Compute>(device().get(), _shader, "ComputeIndirectDraw");
        const auto metaData = _meshInstance->getIndirectMetaData();
        std::copy(metaData.begin(), metaData.end(), _params.indirectMetaData);
        spdlog::info("indirect-draw: {} beads; , . speed, [ ] most instances", _instanceCount);
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_COMMA: _speed = std::max(0.0f, _speed - 0.25f); break;
        case SDLK_PERIOD: _speed = std::min(3.0f, _speed + 0.25f); break;
        case SDLK_LEFTBRACKET: _maxInstances = std::max(0, _maxInstances - 100); break;
        case SDLK_RIGHTBRACKET: _maxInstances = std::min(2000, _maxInstances + 100); break;
        default: return false;
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * _speed;
        if (!_compute) {
            return;
        }
        // A slot in the indirect draw buffer, taken again every frame, which the mesh instance
        // draws from for every camera (null).
        const int slot = device()->getIndirectDrawSlot();
        if (slot < 0) {
            return;
        }
        _meshInstance->setIndirect(nullptr, slot);
        _compute->setParameter("indirectDrawBuffer", device()->indirectDrawBuffer());
        _params.time[0] = _time;
        // No more instances than the buffer holds.
        _params.slots[0] = static_cast<uint32_t>(std::min(_maxInstances, _instanceCount));
        _params.slots[1] = static_cast<uint32_t>(slot);
        _compute->setUniformBlock(&_params, sizeof(_params));
        _compute->setupDispatch(1u, 1u, 1u);
        device()->computeDispatch({_compute.get()}, "ComputeIndirectDraw");
    }

private:
    void createLight(const char* color, const float intensity, const Vector3& angles)
    {
        auto* entity = new Entity();
        entity->setName("Light");
        entity->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(entity->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            light->setColor(Color().fromString(color));
            light->setIntensity(intensity);
        }
        entity->setLocalEulerAngles(angles.getX(), angles.getY(), angles.getZ());
        root()->addChild(entity);
    }

    std::unique_ptr<Asset> _envAsset;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<VertexBuffer> _instances;
    std::shared_ptr<Shader> _shader;
    std::unique_ptr<Compute> _compute;
    MeshInstance* _meshInstance = nullptr;
    IndirectDrawParams _params{};
    int _instanceCount = 0;
    int _maxInstances = 2000;
    float _speed = 1.0f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(IndirectDrawExample)
