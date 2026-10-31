// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream compute/texture-gen.
//
// Two compute shaders write two storage textures every frame, both dispatched in one compute
// pass. Each reads the seaside rocks colour texture by texel, tints it (the first toward
// yellow-green, the second toward magenta, the free channel following sin(t)) and scrolls rings of
// darkness over it from the corner (sin(offset + frequency * distance) * 0.2 + 0.8, the offset
// moving at 20 and 10 a second, the frequency 0.1 and 0.03). Each texture is the diffuse map of an
// icosahedron (gloss 0.6, metalness 0.4, a quarter size) at (0, 1, 0) and (0, -1, 0), rotating 10,
// 20 and 30 degrees a second about its axes, and is shown on the right of the window. A directional
// light turned 45 degrees about X; the helipad environment at skybox mip 1, intensity 3; the camera
// at (0.6, 0, 5) clears to (0.5, 0.6, 0.9) and tone-maps ACES. The performance HUD shows the
// frame's compute dispatches and passes.
//
// DEVIATIONS:
// - The kernel is Slang, its bindings at the indices compute.h names: the textures by name
//   (Metal texture 0 / 1, Vulkan 0 / 1), the uniform block after them (Metal buffer 0, Vulkan 2).
//   The input is read through a combined sampler on Vulkan and as a plain texture on Metal.
// - The rotation is applied in the update, where upstream gives each icosahedron a script.
// - The previews are TexturePreviewPasses (examples/texturePreview.h), upstream's TextureRenderer.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "../texturePreview.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    // Tints the input texture and writes the result to the storage texture.
    constexpr const char* kTextureGenSlang = R"slang(
struct TextureGenParams
{
    float4 tint;
    float4 wave;    // x: offset, y: frequency
};

#if defined(VT_TARGET_METAL)
[[vk::binding(0, 0)]] Texture2D<float4> inTexture : register(t0);
#else
[[vk::binding(0, 0)]] Sampler2D<float4> inTexture : register(t0);
#endif
[[vk::binding(1, 0)]] [vk::image_format("rgba8")] RWTexture2D<float4> outTexture : register(u1);
[[vk::binding(2, 0)]] ConstantBuffer<TextureGenParams> params : register(b0);

[shader("compute")]
[numthreads(8, 8, 1)]
void textureGenMain(uint3 id : SV_DispatchThreadID)
{
    // a thread for each texel - skip the threads beyond the texture, as the dispatch is rounded up
    // to whole workgroups
    uint2 size;
    outTexture.GetDimensions(size.x, size.y);
    if (any(id.xy >= size)) {
        return;
    }

    // load a color from the source texture and tint it
    float4 texColor = inTexture.Load(int3(id.xy, 0));
    texColor *= params.tint;

    // scroll a darkness effect over the texture
    const float2 uv = float2(id.xy);
    const float darkness = sin(params.wave.x + params.wave.y * length(uv)) * 0.2 + 0.8;
    texColor *= darkness;

    outTexture[id.xy] = texColor;
}
)slang";

    struct TextureGenParams
    {
        float tint[4];
        float wave[4];
    };
}

class TextureGenExample final : public ExampleApp
{
public:
    TextureGenExample() : ExampleApp({.title = "Compute Texture Gen", .miniStatsPresets = {"compute"}}) {}

protected:
    bool create() override
    {
        _textureAsset = std::make_unique<Asset>("color", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-color.jpg"));
        _solidAsset = std::make_unique<Asset>("solid", AssetType::CONTAINER, assetPath("models/icosahedron.glb"));
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _source = _textureAsset->resourceAs<Texture>();
        auto* solid = _solidAsset->resourceAs<ContainerResource>();
        Texture* helipad = _helipadAsset->resourceAs<Texture>();
        if (!_source || !solid || !helipad) {
            spdlog::error("texture-gen needs the seaside rocks colour texture, the icosahedron and the helipad atlas");
            return false;
        }

        // Setup skydome
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(3.0f);
        scene()->setEnvAtlas(helipad);

        Entity* camera = createCamera(Vector3(0.6f, 0.0f, 5.0f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        cameraComponent->camera()->setClearColor(Color(0.5f, 0.6f, 0.9f, 1.0f));
        cameraComponent->setToneMapping(TONEMAP_ACES);

        createDirectionalLight(Vector3(45.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f);

        if (!device()->supportsCompute()) {
            spdlog::warn("texture-gen: this device has no compute; only the scene is drawn");
            return true;
        }
        _shader = getOrCreateSlangShaderFromSource(device().get(), "texture-gen-compute", kTextureGenSlang);
        if (!_shader) {
            spdlog::error("texture-gen: the compute shader did not compile");
            return false;
        }

        // Two instances of an icosahedron and the compute shader that updates its texture
        createInstance(*solid, Vector3(0.0f, 1.0f, 0.0f));
        createInstance(*solid, Vector3(0.0f, -1.0f, 0.0f));
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        for (Entity* solid : _solids) {
            solid->rotate(10.0f * dt, 20.0f * dt, 30.0f * dt);
        }
        if (_instances.size() < 2) {
            return;
        }

        const float wave = std::sin(_time) * 0.5f + 0.5f;
        const TextureGenParams params1{{wave, 1.0f, 0.0f, 1.0f}, {20.0f * _time, 0.1f, 0.0f, 0.0f}};
        const TextureGenParams params2{{1.0f, 0.0f, wave, 1.0f}, {10.0f * _time, 0.03f, 0.0f, 0.0f}};
        _instances[0].compute->setUniformBlock(&params1, sizeof(params1));
        _instances[1].compute->setUniformBlock(&params2, sizeof(params2));

        // Set up both dispatches, a thread for each texel, and dispatch them in a single compute pass
        std::vector<Compute*> computes;
        for (auto& instance : _instances) {
            instance.compute->setupDispatchThreads(_source->width(), _source->height());
            computes.push_back(instance.compute.get());
        }
        device()->computeDispatch(computes, "ComputeModifyTextureDispatch");

        // Show the generated textures on the right
        placePreview(*_instances[0].preview, 0.65f, 0.175f, 0.3f, 0.15f);
        placePreview(*_instances[1].preview, 0.65f, 0.675f, 0.3f, 0.15f);
    }

    void destroy() override
    {
        // The passes are registered with the renderer, so they go while the engine is alive.
        if (engine() && engine()->renderer()) {
            for (const auto& instance : _instances) {
                engine()->renderer()->removeAppendPass(instance.preview);
            }
        }
        _instances.clear();
    }

private:
    struct Instance
    {
        std::shared_ptr<Texture> storage;
        std::unique_ptr<Compute> compute;
        std::shared_ptr<StandardMaterial> material;
        std::shared_ptr<TexturePreviewPass> preview;
    };

    void createInstance(ContainerResource& solidResource, const Vector3& position)
    {
        Instance instance;

        // A storage texture the compute shader writes, of the input texture's size
        TextureOptions options;
        options.name = "outputTexture";
        options.width = _source->width();
        options.height = _source->height();
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        options.storage = true;
        instance.storage = std::make_shared<Texture>(device().get(), options);
        instance.storage->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        instance.storage->setAddressV(ADDRESS_CLAMP_TO_EDGE);

        instance.compute = std::make_unique<Compute>(device().get(), _shader, "ComputeModifyTexture");
        instance.compute->setParameter("inTexture", _source);
        instance.compute->setParameter("outTexture", instance.storage.get());

        // The icosahedron, the storage texture its diffuse map
        instance.material = std::make_shared<StandardMaterial>();
        instance.material->setDiffuseMap(instance.storage.get());
        instance.material->setGloss(0.6f);
        instance.material->setMetalness(0.4f);
        instance.material->setUseMetalness(true);

        Entity* solid = solidResource.instantiateRenderEntity();
        if (auto* object = solid->findByName("Object_3")) {
            if (auto* render = static_cast<Entity*>(object)->findComponent<RenderComponent>();
                render && !render->meshInstances().empty()) {
                render->meshInstances()[0]->setMaterial(instance.material);
            }
        }
        solid->setLocalPosition(position);
        solid->setLocalScale(0.25f, 0.25f, 0.25f);
        root()->addChild(solid);
        _solids.push_back(solid);

        instance.preview = std::make_shared<TexturePreviewPass>(device(), instance.storage.get());
        instance.preview->init(nullptr);
        engine()->renderer()->addAppendPass(instance.preview);
        _instances.push_back(std::move(instance));
    }

    // A preview at (x, y) with the size (w, h), as fractions of the back buffer, from its top left.
    void placePreview(TexturePreviewPass& preview, const float x, const float y, const float w, const float h) const
    {
        const auto [width, height] = device()->size();
        const auto fw = static_cast<float>(width);
        const auto fh = static_cast<float>(height);
        preview.setViewport(Vector4(x * fw, y * fh, w * fw, h * fh));
        preview.setScissor(preview.viewport());
    }

    std::unique_ptr<Asset> _textureAsset;
    std::unique_ptr<Asset> _solidAsset;
    std::unique_ptr<Asset> _helipadAsset;
    Texture* _source = nullptr;
    std::shared_ptr<Shader> _shader;
    std::vector<Instance> _instances;
    std::vector<Entity*> _solids;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TextureGenExample)
