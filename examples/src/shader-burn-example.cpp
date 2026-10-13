// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream shaders/shader-burn.
//
// The statue burns away and back again. Every mesh instance of it is drawn with one
// ShaderMaterial: the fragment stage reads a height from the red channel of the clouds texture
// at the mesh's uv, discards the fragment where the height is below a threshold, paints a band
// just above it a glowing (5, 0.02, 0) and otherwise shows the statue's own diffuse map (the
// first mesh instance's), unlit, decoded and gamma-encoded again. The threshold climbs from 0 to
// 1 at 0.2 a second and comes back down, a ten-second cycle. The camera at (0, 7, 24) looks down
// -Z and clears to (0.4, 0.45, 0.5); an ambient of 0.2 and a white omni light of range 10 at
// (0, 1, 0) are in the scene, though the shader does no lighting. No controls.
//
// DEVIATION: the shader is Slang (one source for both backends), a ShaderMaterial over
// getOrCreateSlangShaderFromSource; the diffuse map is the material's base colour map, the
// height map its normal map slot, and the threshold travels in the material's own uniform
// block. The diffuse map is stored raw and decoded in the shader with the sRGB curve, where
// upstream samples an sRGB texture.
// DEVIATION: upstream asks for an HDR display format, so on an HDR display its burning band
// (red 5 before the gamma encode) shows brighter than white. The back buffer here is SDR, and
// the band clips.
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/texture.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    const char* const kBurnSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct BurnData
{
    float4 time;   // x: the height below which the statue has burnt away
};
VT_DECLARE_MATERIAL_UNIFORMS(BurnData, burn)
VT_DECLARE_baseColorMap   // the statue's diffuse map
VT_DECLARE_normalMap      // the height map

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 uv0 : TEXCOORD0;
};

[shader("vertex")]
Varyings burnVertex(VertexIn input)
{
    Varyings output;
    output.uv0 = input.uv0;
    float4 clip = mul(mul(float4(input.position, 1.0), drawModel()), drawViewProjection());
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    return output;
}

// The sRGB decode a texture sampled through an sRGB format gets.
float3 srgbToLinear(float3 c)
{
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, float3(2.4, 2.4, 2.4)));
}

[shader("fragment")]
float4 burnFragment(Varyings input) : SV_Target0
{
    const float uTime = burn.time.x;
    const float height = normalMap.Sample(input.uv0).r;
    float3 linearColor = srgbToLinear(baseColorMap.Sample(input.uv0).rgb);
    if (height < uTime) {
        discard;
    }
    if (height < (uTime + uTime * 0.1)) {
        linearColor = float3(5.0, 0.02, 0.0);
    }
    // gammaCorrectOutput
    return float4(pow(linearColor + 0.0000001, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2)), 1.0);
}
)slang";

    class BurnMaterial final : public ShaderMaterial
    {
    public:
        using ShaderMaterial::ShaderMaterial;

        void setTime(const float time)
        {
            // A block the material packs is reused until it says it changed.
            markUniformsDirty();
            _data.time[0] = time;
        }

        const void* customUniformData(size_t& outSize) const override
        {
            outSize = sizeof(_data);
            return &_data;
        }

    private:
        struct alignas(16) Data
        {
            float time[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        } _data;
    };
}

class ShaderBurnExample final : public ExampleApp
{
public:
    ShaderBurnExample() : ExampleApp({.title = "Shader Burn"}) {}

protected:
    bool create() override
    {
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _cloudsAsset = std::make_unique<Asset>("clouds", AssetType::TEXTURE, assetPath("textures/clouds.jpg"));
        auto* statue = _statueAsset->resourceAs<ContainerResource>();
        Texture* clouds = _cloudsAsset->resourceAs<Texture>();
        if (!statue || !clouds) {
            spdlog::error("shader-burn needs models/statue.glb and textures/clouds.jpg");
            return false;
        }

        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // The camera
        auto* camera = createCamera(Vector3(0.0f, 7.0f, 24.0f));
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        }

        // An omni light
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            lightComponent->setType(LightType::LIGHTTYPE_OMNI);
            lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            lightComponent->setRange(10.0f);
        }
        light->setLocalPosition(0.0f, 1.0f, 0.0f);
        root()->addChild(light);

        // The material with the burn shader and the height map
        auto shader = getOrCreateSlangShaderFromSource(device().get(), "shader-burn", kBurnSlang);
        if (!shader) {
            spdlog::error("shader-burn: the shader did not compile");
            return false;
        }
        _material = std::make_shared<BurnMaterial>("burn", shader);
        _material->setNormalTexture(clouds);

        // The statue, every mesh instance taking the new material, which uses the original
        // texture of the first one
        auto* entity = statue->instantiateRenderEntity();
        root()->addChild(entity);
        Texture* originalTexture = nullptr;
        for (auto* render : entity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                if (!originalTexture && meshInstance->material()) {
                    originalTexture = meshInstance->material()->baseColorTexture();
                }
                meshInstance->setMaterial(_material.get());
            }
        }
        _material->setBaseColorTexture(originalTexture);
        return true;
    }

    void update(const float dt) override
    {
        _time += 0.2f * dt;

        // Reverse time
        float t = std::fmod(_time, 2.0f);
        if (t > 1.0f) {
            t = 1.0f - (t - 1.0f);
        }
        _material->setTime(t);
    }

    void destroy() override
    {
        if (_material) {
            _material->setBaseColorTexture(nullptr);
            _material->setNormalTexture(nullptr);
        }
    }

private:
    std::unique_ptr<Asset> _statueAsset;
    std::unique_ptr<Asset> _cloudsAsset;
    std::shared_ptr<BurnMaterial> _material;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ShaderBurnExample)
