// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream shaders/grab-pass.
//
// A frosted glass box over a ring of seven shapes, refracting a copy of the frame. The shapes
// (box, cone, cylinder, sphere or capsule, picked at random, scaled (4, 8, 4) and spaced evenly
// on a circle of radius 12) have random diffuse colours, gloss 0.6 and metalness 0.4, and are lit
// only by the helipad environment (skybox mip 0, exposure 2), which is also the sky. The depth
// layer, where the camera copies the frame into its scene colour map, is moved after the world's
// opaque sublayer and the skybox, so the copy holds both. The glass, a box of 10 at (1, 3, 0),
// is drawn after the copy with a ShaderMaterial (blended, in the transparent sublayer): it reads
// the copy at its own screen position, offset by a normal map, at a mip level 5 x its roughness
// (1 minus the red of a grey roughness texture), tinted red, white or blue by that level,
// brightened by 1.1 and lifted by roughness x 0.09. The shapes turn by (0.3, 0.2, 0.1) degrees a
// frame and the glass by (-0.1, 0.1, -0.15); the camera circles the scene at radius 20 and
// height 7, 0.2 radians a second, looking at (0, 2, 0), with ACES tone mapping. No controls.
//
// DEVIATION: the shader is Slang (one source for both backends), a ShaderMaterial over
// getOrCreateSlangShaderFromSource: the offset map is the material's base colour map, the
// roughness map its normal map slot, and the tints and the screen size travel in the material's
// own uniform block. The copy is read with this engine's top-left screen origin, as upstream's
// WebGPU shader reads it, so the distortion runs vertically as it does there.
//
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    // The refraction: the scene colour copy, offset and blurred by the glass's maps.
    const char* const kRefractionSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct RefractionData
{
    float4 tints[4];     // rgb: the tint for each mip level
    float4 screenSize;   // xy: the size of the target in pixels, zw: its inverse
};
VT_DECLARE_MATERIAL_UNIFORMS(RefractionData, refraction)
VT_DECLARE_baseColorMap   // the offset map (a normal map)
VT_DECLARE_normalMap      // the roughness map
VT_DECLARE_sceneColorGrab
// linear, linear mips, clamp: the sampler the forward pass reads the copy through
[[vk::binding(12, 3)]] SamplerState linearClampSampler : register(s7);

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

[shader("vertex")]
Varyings refractionVertex(VertexIn input)
{
    Varyings output;
    // project the position
    const float4 pos = mul(float4(input.position, 1.0), drawModel());
    float4 clip = mul(pos, drawViewProjection());
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    output.texCoord = input.uv0;
    return output;
}

[shader("fragment")]
float4 refractionFragment(Varyings input) : SV_Target0
{
    const float roughness = 1.0 - normalMap.Sample(input.texCoord).r;

    // sample offset texture - used to add distortion to the sampled background
    float2 offset = baseColorMap.Sample(input.texCoord).rg;
    offset = 2.0 * offset - 1.0;

    // offset strength
    offset *= (0.2 + roughness) * 0.015;

    // get normalized uv coordinates for canvas
    const float2 grabUv = input.position.xy * refraction.screenSize.zw;

    // roughness dictates which mipmap level gets used, in 0..4 range
    const float mipmap = roughness * 5.0;

    // get background pixel color with distorted offset
    float3 grabColor = sceneColorGrab.SampleLevel(linearClampSampler, grabUv + offset, mipmap).rgb;

    // tint the material based on mipmap
    const float tintIndex = clamp(mipmap, 0.0, 3.0);
    grabColor *= refraction.tints[int(tintIndex)].rgb;

    // brighten the refracted texture a little bit
    // brighten even more the rough parts of the glass
    return float4(grabColor * 1.1, 1.0) + roughness * 0.09;
}
)slang";

    class RefractionMaterial final : public ShaderMaterial
    {
    public:
        using ShaderMaterial::ShaderMaterial;

        void setTint(const int index, const float r, const float g, const float b)
        {
            markUniformsDirty();
            _data.tints[index][0] = r;
            _data.tints[index][1] = g;
            _data.tints[index][2] = b;
        }

        void setScreenSize(const float width, const float height)
        {
            if (_data.screenSize[0] == width && _data.screenSize[1] == height) {
                return;
            }
            // A block the material packs is reused until it says it changed.
            markUniformsDirty();
            _data.screenSize[0] = width;
            _data.screenSize[1] = height;
            _data.screenSize[2] = width > 0.0f ? 1.0f / width : 0.0f;
            _data.screenSize[3] = height > 0.0f ? 1.0f / height : 0.0f;
        }

        const void* customUniformData(size_t& outSize) const override
        {
            outSize = sizeof(_data);
            return &_data;
        }

    private:
        struct alignas(16) Data
        {
            float tints[4][4] = {};
            float screenSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        } _data;
    };
}

class GrabPassExample final : public ExampleApp
{
public:
    GrabPassExample() : ExampleApp({.title = "Grab Pass"}) {}

protected:
    bool create() override
    {
        _normal = std::make_unique<Asset>("normal", AssetType::TEXTURE, assetPath("textures/normal-map.png"));
        _roughness = std::make_unique<Asset>("roughness", AssetType::TEXTURE, assetPath("textures/pc-gray.png"));
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* normal = _normal->resourceAs<Texture>();
        Texture* roughness = _roughness->resourceAs<Texture>();
        Texture* helipad = _helipad->resourceAs<Texture>();
        if (!normal || !roughness || !helipad) {
            spdlog::error("grab-pass needs textures/normal-map.png, textures/pc-gray.png and the helipad env atlas");
            return false;
        }

        // The skydome
        scene()->setSkyboxMip(0);
        scene()->setExposure(2.0f);
        scene()->setEnvAtlas(helipad);

        // The depth layer is where the frame is copied to a texture for the following layers.
        // Move it after the world's opaque sublayer and the skybox, so the copy holds both.
        if (const auto layers = scene()->layers()) {
            if (const auto depthLayer = layers->getLayerById(LAYERID_DEPTH)) {
                layers->remove(depthLayer);
                layers->insertOpaque(depthLayer, 2);
            }
        }

        // A few shapes to turn, of a random shape and colour, on a circle
        constexpr int count = 7;
        const std::array<const char*, 5> shapes = {"box", "cone", "cylinder", "sphere", "capsule"};
        std::mt19937 rng(12345u);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        for (int i = 0; i < count; ++i) {
            const char* shapeName = shapes[static_cast<size_t>(unit(rng) * shapes.size()) % shapes.size()];
            const float r = unit(rng);
            const float g = unit(rng);
            const float b = unit(rng);
            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(count);
            _primitives.push_back(createShape(shapeName, Vector3(12.0f * std::sin(angle), 0.0f, 12.0f * std::cos(angle)),
                Vector3(4.0f, 8.0f, 4.0f), Color(r, g, b, 1.0f)));
        }

        // The camera, which renders the scene and copies its colour for the glass
        _camera = createCamera(Vector3(0.0f, 10.0f, 20.0f));
        _camera->setName("SceneCamera");
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        if (auto* cameraComponent = _camera->findComponent<CameraComponent>()) {
            cameraComponent->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
            cameraComponent->setToneMapping(TONEMAP_ACES);
            cameraComponent->requestSceneColorMap(true);
        }

        // The glass: a box drawn with the refraction shader
        auto shader = getOrCreateSlangShaderFromSource(device().get(), "grab-pass-refraction", kRefractionSlang);
        if (!shader) {
            spdlog::error("grab-pass: the refraction shader did not compile");
            return false;
        }
        _refractionMaterial = std::make_shared<RefractionMaterial>("RefractionShader", shader);
        // The offset map, the roughness map
        _refractionMaterial->setBaseColorTexture(normal);
        _refractionMaterial->setNormalTexture(roughness);
        // The tints: red, white, blue, white
        _refractionMaterial->setTint(0, 1.0f, 0.7f, 0.7f);
        _refractionMaterial->setTint(1, 1.0f, 1.0f, 1.0f);
        _refractionMaterial->setTint(2, 0.7f, 0.7f, 1.0f);
        _refractionMaterial->setTint(3, 1.0f, 1.0f, 1.0f);
        // Transparency: blended, with depth writes left on
        _refractionMaterial->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        _refractionMaterial->setTransparent(true);

        _glass = createPrimitive("box", _refractionMaterial.get(), Vector3(1.0f, 3.0f, 0.0f),
            Vector3(10.0f, 10.0f, 10.0f));
        if (auto* render = _glass->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Turn the shapes and the glass
        for (auto* primitive : _primitives) {
            primitive->rotate(0.3f, 0.2f, 0.1f);
        }
        _glass->rotate(-0.1f, 0.1f, -0.15f);

        // Orbit the camera
        _camera->setLocalPosition(20.0f * std::sin(_time * 0.2f), 7.0f, 20.0f * std::cos(_time * 0.2f));
        _camera->lookAt(Vector3(0.0f, 2.0f, 0.0f));

        const auto [width, height] = device()->size();
        _refractionMaterial->setScreenSize(static_cast<float>(width), static_cast<float>(height));
    }

    void destroy() override
    {
        if (_refractionMaterial) {
            _refractionMaterial->setBaseColorTexture(nullptr);
            _refractionMaterial->setNormalTexture(nullptr);
        }
    }

private:
    // A shape of a type, position, scale and colour.
    Entity* createShape(const char* type, const Vector3& position, const Vector3& scale, const Color& color)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setGloss(0.6f);
        material->setMetalness(0.4f);
        material->setUseMetalness(true);
        _materials.push_back(material);
        return createPrimitive(type, material.get(), position, scale);
    }

    std::unique_ptr<Asset> _normal;
    std::unique_ptr<Asset> _roughness;
    std::unique_ptr<Asset> _helipad;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<RefractionMaterial> _refractionMaterial;
    std::vector<Entity*> _primitives;
    Entity* _glass = nullptr;
    Entity* _camera = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(GrabPassExample)
