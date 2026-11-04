// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream compute/texture-gen.
//
// Two trefoil knots, each painted with a texture a compute shader generates every frame, both
// dispatched in one compute pass. The kernel bends the texel's position with a few waves that
// repeat a whole number of times across the texture (so it tiles without seams), colours it from a
// cosine palette flowing over time with thin bright contour lines, and lays the white of the
// upstream logo (read by texel, the white on grey) over it. The second knot's texture uses the
// opposite colours of the palette and flows slower. The textures repeat along the knots, as
// their diffuse and emissive maps (glossy paint, gloss 0.7, that also glows a little); the knots
// turn slowly in opposite directions. The product studio's environment (intensity 0.3) under a
// camera rendering no skybox against a dark clear colour; a soft key light and two rim lights.
// The camera frame (4x MSAA, neutral tone mapping) adds bloom and a vignette; the textures show
// side by side in the bottom right corner; the HUD shows the compute dispatches and passes.
//
// Keys stand in for upstream's controls panel:
//   , / .        the speed -/+ 0.25 (0 - 3)
//   [ / ]        the hue -/+ 0.05 (0 - 1)
//
// DEVIATIONS:
// - The kernel is Slang, its bindings at the indices compute.h names: the textures by name
//   (Metal texture 0 / 1, Vulkan 0 / 1), the uniform block after them (Metal buffer 0, Vulkan 2).
//   The input is read through a combined sampler on Vulkan and as a plain texture on Metal.
// - The kernel stores the colour gamma encoded, which the material decodes as every 8-bit colour
//   map here (upstream stores pow(colour, 2.2) and reads the texture as linear).
// - The previews are TexturePreviewPasses (examples/texturePreview.h), upstream's TextureRenderer;
//   there is no "Generated textures" caption above them.
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "../texturePreview.h"
#include "framework/assets/asset.h"
#include "extras/script/cameraControls.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
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
    // Generates a flowing, tiling pattern with the logo laid over it.
    constexpr const char* kTextureGenSlang = R"slang(
struct TextureGenParams
{
    float4 params;    // x: time, y: hue
};

#if defined(VT_TARGET_METAL)
[[vk::binding(0, 0)]] Texture2D<float4> inTexture : register(t0);
#else
[[vk::binding(0, 0)]] Sampler2D<float4> inTexture : register(t0);
#endif
[[vk::binding(1, 0)]] [vk::image_format("rgba8")] RWTexture2D<float4> outTexture : register(u1);
[[vk::binding(2, 0)]] ConstantBuffer<TextureGenParams> params : register(b0);

static const float TAU = 6.2831853;

// A palette of smoothly changing colours, a cosine wave for each channel.
float3 palette(float t)
{
    return float3(0.55, 0.35, 0.6) + float3(0.45, 0.35, 0.4) * cos(TAU * (t + float3(0.0, 0.15, 0.3)));
}

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

    // the texel's position, an angle from 0 to 2 PI in both directions
    const float2 p = (float2(id.xy) + 0.5) / float2(size) * TAU;
    const float t = params.params.x;

    // Bend it with a few waves, so the pattern flows; they repeat a whole number of times across
    // the texture, so it tiles without seams.
    float2 q = p;
    q += 0.7 * float2(sin(2.0 * q.y + t), sin(3.0 * q.x - 0.7 * t));
    q += 0.5 * float2(sin(3.0 * q.y - 1.3 * t), sin(2.0 * q.x + 0.9 * t));
    const float v = sin(2.0 * q.x + t) * sin(q.y - 0.5 * t);

    // the colour flows through the palette, with thin bright lines where the pattern crosses even
    // steps, as the lines of a contour map
    const float hue = params.params.y;
    float3 color = palette(0.5 * v + 0.1 * t + hue) * (0.65 + 0.35 * v);
    const float line = abs(frac(v * 2.0) - 0.5);
    color += palette(0.5 * v + 0.1 * t + hue + 0.5) * (1.0 - smoothstep(0.0, 0.12, line));

    // The logo over the pattern: only its white, the grey around it leaves the pattern as it is.
    const float logo = inTexture.Load(int3(id.xy, 0)).r;
    color = lerp(color, float3(1.0, 1.0, 1.0), smoothstep(0.75, 0.95, logo) * 0.9);

    // Stored gamma encoded: the material decodes an 8-bit colour map.
    outTexture[id.xy] = float4(saturate(color), 1.0);
}
)slang";

    struct TextureGenParams
    {
        float params[4];
    };
}

class TextureGenExample final : public ExampleApp
{
public:
    TextureGenExample() : ExampleApp({.title = "Compute Texture Gen", .miniStatsPresets = {"compute"}}) {}

protected:
    bool create() override
    {
        _logoAsset = std::make_unique<Asset>("logo", AssetType::TEXTURE, assetPath("textures/playcanvas-grey.png"));
        _knotAsset = std::make_unique<Asset>("knot", AssetType::CONTAINER, assetPath("models/trefoil-knot.glb"));
        _envAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/product-studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _source = _logoAsset->resourceAs<Texture>();
        auto* knot = _knotAsset->resourceAs<ContainerResource>();
        Texture* env = _envAsset->resourceAs<Texture>();
        if (!_source || !knot || !env) {
            spdlog::error("texture-gen needs the logo texture, the trefoil knot and the product studio atlas");
            return false;
        }
        scene()->setEnvAtlas(env);
        scene()->setSkyboxIntensity(0.3f);

        // The camera renders no skybox: the background is the clear colour (linear, as the camera
        // frame reads it).
        Entity* camera = createCamera(Vector3(0.0f, 0.5f, 6.5f));
        auto* cameraComponent = camera->findComponent<CameraComponent>();
        Color clear;
        clear.fromString("#0c0e14");
        clear.linear();
        cameraComponent->camera()->setClearColor(clear);
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_IMMEDIATE, LAYERID_UI});
        cameraComponent->setToneMapping(TONEMAP_NEUTRAL);
        if (CameraControls* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(3.0f, 15.0f));
            controls->storeResetState();
        }
        auto rendering = cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 6;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.5f;
        cameraComponent->setRendering(rendering);

        // A soft key light from the upper left and two rim lights from behind the knots.
        createLight("#fff1e0", 1.2f, Vector3(45.0f, -35.0f, 0.0f));
        createLight("#ffffff", 0.8f, Vector3(-60.0f, 120.0f, 0.0f));
        createLight("#ffffff", 0.8f, Vector3(-60.0f, -120.0f, 0.0f));

        if (!device()->supportsCompute()) {
            spdlog::warn("texture-gen: this device has no compute; only the scene is drawn");
            return true;
        }
        _shader = getOrCreateSlangShaderFromSource(device().get(), "texture-gen-compute", kTextureGenSlang);
        if (!_shader) {
            spdlog::error("texture-gen: the compute shader did not compile");
            return false;
        }

        // Two knots, each with a texture its own compute instance generates; the second in the
        // opposite colours of the palette, flowing slower.
        createKnot(*knot, -1.4f, 0.0f, 1.0f);
        createKnot(*knot, 1.4f, 0.5f, 0.6f);
        spdlog::info("texture-gen: , . speed, [ ] hue");
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
        case SDLK_LEFTBRACKET: _hue = std::max(0.0f, _hue - 0.05f); break;
        case SDLK_RIGHTBRACKET: _hue = std::min(1.0f, _hue + 0.05f); break;
        default: return false;
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * _speed;

        // The previews are square, a fifth of the screen's height, side by side in the bottom right.
        const auto [width, height] = device()->size();
        const float previewHeight = 0.2f;
        const float previewWidth = previewHeight * static_cast<float>(height) / static_cast<float>(std::max(width, 1));

        std::vector<Compute*> computes;
        for (size_t i = 0; i < _instances.size(); ++i) {
            Instance& instance = _instances[i];
            const TextureGenParams params{{_time * instance.speed, _hue + instance.hue, 0.0f, 0.0f}};
            instance.compute->setUniformBlock(&params, sizeof(params));
            // A thread for each texel.
            instance.compute->setupDispatchThreads(instance.storage->width(), instance.storage->height());
            computes.push_back(instance.compute.get());

            // The knots turn slowly, in opposite directions.
            const float direction = i == 0 ? 1.0f : -1.0f;
            instance.knot->setLocalEulerAngles(_time * 5.0f, direction * _time * 12.0f, 0.0f);

            const float fi = static_cast<float>(i);
            placePreview(*instance.preview, 0.975f - (2.0f - fi) * previewWidth - (1.0f - fi) * 0.01f, 0.775f,
                previewWidth, previewHeight);
        }
        // Both dispatches in a single compute pass.
        if (!computes.empty()) {
            device()->computeDispatch(computes, "ComputeGenerateTextureDispatch");
        }
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
        Entity* knot = nullptr;
        float hue = 0.0f;
        float speed = 1.0f;
    };

    void createKnot(ContainerResource& knotResource, const float x, const float hue, const float speed)
    {
        Instance instance;
        instance.hue = hue;
        instance.speed = speed;

        // The storage texture, the logo's size so the kernel reads and writes the same texel; it
        // repeats along the knot.
        TextureOptions options;
        options.name = "GeneratedTexture";
        options.width = _source->width();
        options.height = _source->height();
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        options.storage = true;
        instance.storage = std::make_shared<Texture>(device().get(), options);
        instance.storage->setAddressU(ADDRESS_REPEAT);
        instance.storage->setAddressV(ADDRESS_REPEAT);

        instance.compute = std::make_unique<Compute>(device().get(), _shader, "ComputeGenerateTexture");
        instance.compute->setParameter("inTexture", _source);
        instance.compute->setParameter("outTexture", instance.storage.get());

        // Glossy paint in the texture's colours, which also glows a little with them.
        instance.material = std::make_shared<StandardMaterial>();
        instance.material->setDiffuseMap(instance.storage.get());
        instance.material->setEmissiveMap(instance.storage.get());
        instance.material->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        instance.material->setEmissiveIntensity(1.0f);
        instance.material->setGloss(0.7f);
        instance.material->setUseMetalness(true);
        instance.material->setMetalness(0.0f);

        instance.knot = knotResource.instantiateRenderEntity();
        instance.knot->setEngine(engine());
        for (auto* render : instance.knot->findComponents<RenderComponent>()) {
            if (!render->meshInstances().empty()) {
                render->meshInstances()[0]->setMaterial(instance.material);
            }
        }
        instance.knot->setLocalPosition(x, 0.0f, 0.0f);
        instance.knot->setLocalScale(0.6f, 0.6f, 0.6f);
        root()->addChild(instance.knot);

        instance.preview = std::make_shared<TexturePreviewPass>(device(), instance.storage.get());
        instance.preview->init(nullptr);
        engine()->renderer()->addAppendPass(instance.preview);
        _instances.push_back(std::move(instance));
    }

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

    // A preview at (x, y) with the size (w, h), as fractions of the back buffer, from its top left.
    void placePreview(TexturePreviewPass& preview, const float x, const float y, const float w, const float h) const
    {
        const auto [width, height] = device()->size();
        const auto fw = static_cast<float>(width);
        const auto fh = static_cast<float>(height);
        preview.setViewport(Vector4(x * fw, y * fh, w * fw, h * fh));
        preview.setScissor(preview.viewport());
    }

    std::unique_ptr<Asset> _logoAsset;
    std::unique_ptr<Asset> _knotAsset;
    std::unique_ptr<Asset> _envAsset;
    Texture* _source = nullptr;
    std::shared_ptr<Shader> _shader;
    std::vector<Instance> _instances;
    float _speed = 1.0f;
    float _hue = 0.4f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TextureGenExample)
