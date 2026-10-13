// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream shaders/shader-material.
//
// A unit box at the origin drawn with a ShaderMaterial of its own: the vertex stage passes the
// box's object-space position remapped from [-0.5, 0.5] towards [0, 1] as a colour, and the
// fragment stage multiplies the logo texture (sampled raw, as a non-sRGB texture asset) by that
// colour plus an animated `amount`, a sine running from 0 to 1 four radians a second. The output
// is written as it is, with no gamma encoding. The box turns by euler (10, 20, 30) degrees a
// second in world space; the camera at (0, 0, 3) looks down -Z and clears to (0.5, 0.6, 0.9).
// There is no light (the shader does no lighting) and there are no controls.
//
// DEVIATION: the shader is Slang (one source for both backends), a ShaderMaterial over
// getOrCreateSlangShaderFromSource; the texture is the material's base colour map and `amount`
// travels in the material's own uniform block.
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/graphics/texture.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    const char* const kShaderSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct ShaderData
{
    float4 amount;   // x: the tint added to the position colour
};
VT_DECLARE_MATERIAL_UNIFORMS(ShaderData, data)
VT_DECLARE_baseColorMap

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
};

struct Varyings
{
    float4 position : SV_Position;
    float4 fragPosition : TEXCOORD0;
    float2 uv0 : TEXCOORD1;
};

[shader("vertex")]
Varyings shaderVertex(VertexIn input)
{
    Varyings output;
    const float4 localPos = float4(input.position, 1.0);
    float4 clip = mul(mul(localPos, drawModel()), drawViewProjection());
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    output.fragPosition = 0.5 * (localPos + float4(1.0, 1.0, 1.0, 1.0));
    output.uv0 = input.uv0;
    return output;
}

[shader("fragment")]
float4 shaderFragment(Varyings input) : SV_Target0
{
    const float3 color = input.fragPosition.rgb;
    const float3 tint = float3(data.amount.x, data.amount.x, data.amount.x) + color;
    const float4 diffuseColor = baseColorMap.Sample(input.uv0);
    return float4(diffuseColor.xyz * tint, 1.0);
}
)slang";

    class ExampleShaderMaterial final : public ShaderMaterial
    {
    public:
        using ShaderMaterial::ShaderMaterial;

        void setAmount(const float amount)
        {
            // A block the material packs is reused until it says it changed.
            markUniformsDirty();
            _data.amount[0] = amount;
        }

        const void* customUniformData(size_t& outSize) const override
        {
            outSize = sizeof(_data);
            return &_data;
        }

    private:
        struct alignas(16) Data
        {
            float amount[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        } _data;
    };
}

class ShaderMaterialExample final : public ExampleApp
{
public:
    ShaderMaterialExample() : ExampleApp({.title = "Shader Material"}) {}

protected:
    bool create() override
    {
        _diffuse = std::make_unique<Asset>("color", AssetType::TEXTURE, assetPath("textures/playcanvas.png"));
        Texture* diffuse = _diffuse->resourceAs<Texture>();
        if (!diffuse) {
            spdlog::error("shader-material needs textures/playcanvas.png");
            return false;
        }

        auto shader = getOrCreateSlangShaderFromSource(device().get(), "shader-material-example", kShaderSlang);
        if (!shader) {
            spdlog::error("shader-material: the shader did not compile");
            return false;
        }
        _material = std::make_shared<ExampleShaderMaterial>("ShaderMaterialExample", shader);
        _material->setBaseColorTexture(diffuse);

        // The box
        _box = createPrimitive("box", _material.get());
        _box->setName("cube");

        // The camera
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 3.0f));
        camera->setName("camera");
        if (auto* cameraComponent = camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.5f, 0.6f, 0.9f, 1.0f));
        }
        return true;
    }

    void update(const float dt) override
    {
        _box->rotate(10.0f * dt, 20.0f * dt, 30.0f * dt);

        // The amount, a sine wave varying from 0 to 1, animates the colour
        _time += dt;
        _material->setAmount((std::sin(_time * 4.0f) + 1.0f) * 0.5f);
    }

    void destroy() override
    {
        if (_material) {
            _material->setBaseColorTexture(nullptr);
        }
    }

private:
    std::unique_ptr<Asset> _diffuse;
    std::shared_ptr<ExampleShaderMaterial> _material;
    Entity* _box = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ShaderMaterialExample)
