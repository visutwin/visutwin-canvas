// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/reflection-planar.
//
// A planar mirror made of two cameras: a reflection camera, mirrored through the ground plane
// every frame, renders the world and the sky into a texture first, and the ground, on a layer
// of its own that the reflection camera does not render, draws that texture with a custom
// shader that samples it at the fragment's screen position, darkened to 40%. The statue stands
// on the mirror, six shapes orbit and spin above it, and the camera circles the scene while its
// field of view breathes between 40 and 80 degrees; the helipad sky (mip 1, intensity 1.7) with
// ACES tone mapping on both cameras.
//
// DEVIATIONS:
// - The shapes and their colours come from a fixed-seed generator (mulberry32, seed 1) rather
//   than Math.random(), so every run has the same layout.
// - The reflection texture is RGBA8 holding the reflection camera's gamma-encoded output (where
//   upstream renders into an sRGB texture), so the ground shader decodes it before darkening.
// - The ground shader is Slang (one source for both backends): a ShaderMaterial over
//   getOrCreateSlangShaderFromSource, the reflection bound as the material's base colour map.
// - Upstream's planarRenderer script is the update() below: the same reflection of the camera
//   through the plane, the same projection copied over.
//
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    // The layer of the ground, which the reflection camera does not render.
    constexpr int LAYERID_EXCLUDED = 100;

    // The ground: the reflection texture sampled at the fragment's position on the screen.
    const char* const kGroundSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct GroundData
{
    float4 screenSize;   // xy: the size of the target in pixels, zw: its inverse
};
VT_DECLARE_MATERIAL_UNIFORMS(GroundData, ground)
VT_DECLARE_baseColorMap

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct Varyings
{
    float4 position : SV_Position;
};

[shader("vertex")]
Varyings groundVertex(VertexIn input)
{
    Varyings output;
    float4 clip = mul(float4(input.position, 1.0), mul(drawModel(), drawViewProjection()));
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    return output;
}

[shader("fragment")]
float4 groundFragment(Varyings input) : SV_Target0
{
    // The mirrored camera is upright (aimed with lookAt), so its image is the reflection
    // upside down: v is flipped back.
    float2 coord = input.position.xy * ground.screenSize.zw;
    coord.y = 1.0 - coord.y;
    const float3 reflection = pow(baseColorMap.Sample(coord).rgb, float3(2.2, 2.2, 2.2));
    const float3 linearColor = reflection * 0.4;
    return float4(pow(linearColor + 0.0000001, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2)), 1.0);
}
)slang";

    class GroundMaterial final : public ShaderMaterial
    {
    public:
        using ShaderMaterial::ShaderMaterial;

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
            float screenSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        } _data;
    };
}

class ReflectionPlanarExample final : public ExampleApp
{
public:
    ReflectionPlanarExample() : ExampleApp({.title = "Planar Reflection"}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _statue = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));

        // The skydome
        if (Texture* envAtlas = _envAtlas->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(1.7f);

        // A layer for what does not render into the reflection, after the world's transparent
        // sublayer
        const auto layers = scene()->layers();
        const auto worldLayer = layers->getLayerById(LAYERID_WORLD);
        auto excludedLayer = std::make_shared<Layer>("Excluded", LAYERID_EXCLUDED);
        layers->insert(excludedLayer, layers->getTransparentIndex(worldLayer) + 1);

        // The reflective ground, on the excluded layer
        auto shader = getOrCreateSlangShaderFromSource(device().get(), "reflection-planar-ground", kGroundSlang);
        if (!shader) {
            spdlog::error("reflection-planar: the ground shader did not compile");
            return false;
        }
        _groundMaterial = std::make_shared<GroundMaterial>("ReflectionGround", shader);
        auto* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(40.0f, 1.0f, 40.0f), {LAYERID_EXCLUDED});
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        // The statue
        if (auto* statue = _statue->resourceAs<ContainerResource>()) {
            root()->addChild(statue->instantiateRenderEntity());
        }

        // Six shapes in the world layer
        const std::array<const char*, 5> shapes = {"box", "cone", "cylinder", "sphere", "capsule"};
        uint32_t seed = 1;
        const auto unit = [&seed] {
            // mulberry32
            seed += 0x6D2B79F5u;
            uint32_t t = (seed ^ (seed >> 15)) * (seed | 1u);
            t = (t + ((t ^ (t >> 7)) * (t | 61u))) ^ t;
            return static_cast<float>(static_cast<double>(t ^ (t >> 14)) / 4294967296.0);
        };
        for (int i = 0; i < 6; ++i) {
            const char* shape = shapes[static_cast<size_t>(unit() * shapes.size()) % shapes.size()];
            const float r = unit();
            const float g = unit();
            const float b = unit();
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(r, g, b, 1.0f));
            material->setGloss(0.6f);
            material->setMetalness(0.7f);
            material->setUseMetalness(true);
            _materials.push_back(material);
            _entities.push_back(createPrimitive(shape, material.get(), Vector3(0.0f, 0.0f, 0.0f),
                Vector3(3.0f, 3.0f, 3.0f), {LAYERID_WORLD}));
        }

        // The reflection camera first: it renders the world and the sky into the texture the
        // ground then reads.
        const auto [width, height] = device()->size();
        TextureOptions textureOptions;
        textureOptions.name = "PlanarReflection";
        textureOptions.width = std::max(width, 1);
        textureOptions.height = std::max(height, 1);
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        _reflectionTexture = std::make_shared<Texture>(device().get(), textureOptions);
        _reflectionTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _reflectionTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device().get();
        targetOptions.colorBuffer = _reflectionTexture.get();
        targetOptions.depth = true;
        targetOptions.name = "PlanarReflectionTarget";
        auto reflectionTarget = device()->createRenderTarget(targetOptions);

        _reflectionCamera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _reflectionCamera->setName("ReflectionCamera");
        _reflectionCameraComponent = _reflectionCamera->findComponent<CameraComponent>();
        _reflectionCameraComponent->setLayers({LAYERID_WORLD, LAYERID_SKYBOX});
        _reflectionCameraComponent->setPriority(-1);
        _reflectionCameraComponent->setToneMapping(TONEMAP_ACES);
        _reflectionCameraComponent->camera()->setRenderTarget(reflectionTarget);

        // The main camera: the world, the ground, the sky and the UI
        _camera = createCamera(Vector3(30.0f, 10.0f, 0.0f));
        _camera->setName("MainCamera");
        _cameraComponent = _camera->findComponent<CameraComponent>();
        _cameraComponent->setLayers({LAYERID_WORLD, LAYERID_EXCLUDED, LAYERID_SKYBOX, LAYERID_UI});
        _cameraComponent->setToneMapping(TONEMAP_ACES);
        _cameraComponent->camera()->setFov(60.0f);

        _groundMaterial->setBaseColorTexture(_reflectionTexture.get());
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // The shapes spin and orbit the statue
        for (size_t e = 0; e < _entities.size(); ++e) {
            const float scale = static_cast<float>(e + 1) / static_cast<float>(_entities.size());
            const float offset = _time + static_cast<float>(e) * 200.0f;
            _entities[e]->setLocalPosition(7.0f * std::sin(offset), static_cast<float>(e) + 5.0f,
                7.0f * std::cos(offset));
            _entities[e]->rotate(1.0f * scale, 2.0f * scale, 3.0f * scale);
        }

        // The camera slowly orbits, its field of view breathing
        _camera->setLocalPosition(30.0f * std::cos(_time * 0.2f), 10.0f, 30.0f * std::sin(_time * 0.2f));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        _cameraComponent->camera()->setFov(60.0f + 20.0f * std::sin(_time * 0.5f));

        // The reflection camera mirrored through the ground plane (y = 0), with the main
        // camera's projection, after the main camera has moved
        const Matrix4 reflection = Matrix4::reflection(0.0f, 1.0f, 0.0f, 0.0f);
        const Vector3 position = _camera->position();
        const Vector3 forward = Vector3(_camera->worldTransform().getColumn(2)) * -1.0f;
        _reflectionCamera->setPosition(reflection.transformPoint(position));
        _reflectionCamera->lookAt(reflection.transformPoint(position + forward));
        Camera* main = _cameraComponent->camera();
        Camera* mirror = _reflectionCameraComponent->camera();
        mirror->setFov(main->fov());
        mirror->setNearClip(main->nearClip());
        mirror->setFarClip(main->farClip());
        mirror->setAspectRatio(main->aspectRatio());

        const auto [width, height] = device()->size();
        _groundMaterial->setScreenSize(static_cast<float>(width), static_cast<float>(height));
    }

    void destroy() override
    {
        if (_groundMaterial) {
            _groundMaterial->setBaseColorTexture(nullptr);
        }
    }

private:
    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _statue;
    std::shared_ptr<GroundMaterial> _groundMaterial;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _entities;
    std::shared_ptr<Texture> _reflectionTexture;
    Entity* _camera = nullptr;
    CameraComponent* _cameraComponent = nullptr;
    Entity* _reflectionCamera = nullptr;
    CameraComponent* _reflectionCameraComponent = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ReflectionPlanarExample)
