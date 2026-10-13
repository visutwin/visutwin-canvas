// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream shaders/shader-wobble.
//
// The statue, every mesh instance of it drawn with one ShaderMaterial that wobbles it: the vertex
// stage moves each world-space position by sin(time + y * 4) * 0.1 along x and then by
// cos(time + x * 4) * 0.1 along y, and the fragment stage writes the statue's own diffuse map
// (the first mesh instance's) unlit, decoded and gamma-encoded again, so it shows as authored.
// The camera at (0, 7, 25) looks down -Z and clears to (0.4, 0.45, 0.5); an ambient of 0.2 and a
// white omni light of range 10 at (0, 1, 0) are in the scene, though the shader does no lighting.
// No controls.
//
// DEVIATION: the shader is Slang (one source for both backends), a ShaderMaterial over
// getOrCreateSlangShaderFromSource; the diffuse map is the material's base colour map and the
// time travels in the material's own uniform block. The map is stored raw and decoded in the
// shader with the sRGB curve, where upstream samples an sRGB texture.
//
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
    const char* const kWobbleSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct WobbleData
{
    float4 time;   // x: seconds
};
VT_DECLARE_MATERIAL_UNIFORMS(WobbleData, wobble)
VT_DECLARE_baseColorMap

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
Varyings wobbleVertex(VertexIn input)
{
    Varyings output;
    float4 pos = mul(float4(input.position, 1.0), drawModel());
    pos.x += sin(wobble.time.x + pos.y * 4.0) * 0.1;
    pos.y += cos(wobble.time.x + pos.x * 4.0) * 0.1;
    output.uv0 = input.uv0;
    float4 clip = mul(pos, drawViewProjection());
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
float4 wobbleFragment(Varyings input) : SV_Target0
{
    const float3 linearColor = srgbToLinear(baseColorMap.Sample(input.uv0).rgb);
    // gammaCorrectOutput
    return float4(pow(linearColor + 0.0000001, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2)), 1.0);
}
)slang";

    class WobbleMaterial final : public ShaderMaterial
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

class ShaderWobbleExample final : public ExampleApp
{
public:
    ShaderWobbleExample() : ExampleApp({.title = "Shader Wobble"}) {}

protected:
    bool create() override
    {
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        auto* statue = _statueAsset->resourceAs<ContainerResource>();
        if (!statue) {
            spdlog::error("shader-wobble needs models/statue.glb");
            return false;
        }

        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        // The camera
        auto* camera = createCamera(Vector3(0.0f, 7.0f, 25.0f));
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

        // The material with the wobble shader
        auto shader = getOrCreateSlangShaderFromSource(device().get(), "shader-wobble", kWobbleSlang);
        if (!shader) {
            spdlog::error("shader-wobble: the shader did not compile");
            return false;
        }
        _material = std::make_shared<WobbleMaterial>("wobble", shader);

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
        _time += dt;
        _material->setTime(_time);
    }

    void destroy() override
    {
        if (_material) {
            _material->setBaseColorTexture(nullptr);
        }
    }

private:
    std::unique_ptr<Asset> _statueAsset;
    std::shared_ptr<WobbleMaterial> _material;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ShaderWobbleExample)
