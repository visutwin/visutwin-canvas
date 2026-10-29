// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// Port of upstream render-targets/texture-array-impostors.
//
// Bakes the statue from 64 directions into the layers of a texture array, using a render target
// per layer, and draws thousands of instanced impostors showing the view closest to the camera
// direction. The views are 16 azimuths at 4 elevations 25 degrees apart, each 512 x 512, rendered
// in the first frame by an orthographic camera per layer (4x MSAA, cleared transparent, ACES)
// framing the statue's bounding sphere, on a layer only they render; each layer's mips are
// generated after it renders. The statue's bake copy, cameras and targets are then destroyed; the
// array stays. A full statue at twice the size stands in the middle of a 60 x 60 jittered grid of
// impostors (scale 0.7..1.3) on a 3000-unit ground, under the helipad environment (skybox mip 1,
// intensity 0.7) and a warm directional light at (50, 30, 0) that lights the bake and the world.
// The camera starts 11 bounding radii out, 14 degrees up, orbiting the full statue (pitch 0..85,
// zoom out to the field's width).
//
// DEVIATIONS:
// - This engine has no sRGB pixel formats: the array is RGBA8 holding the gamma-encoded colour the
//   bake cameras write, so its mips are filtered in gamma space and the impostor shader writes the
//   sample as it is, where upstream's sRGB array is filtered linearly and encoded on output.
// - Instancing takes the matrix format only: each impostor's ground position and scale ride in its
//   instance matrix (a translation and a uniform scale), where upstream uses a four-float attribute.
//   The camera's right and up axes and position reach the shader in the material's uniform block.
// - The shader is Slang (one source for both backends) over the base-colour slot declared as an
//   array.
// - Upstream places the impostors with Math.random; this port uses a fixed-seed std::mt19937.
//
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "framework/components/render/primitiveGeometry.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/composition/layerComposition.h"
#include "scene/layer.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int LAYERID_IMPOSTOR_BAKE = 71;
    constexpr int kAzimuthCount = 16;
    constexpr int kElevationCount = 4;
    constexpr int kFrameCount = kAzimuthCount * kElevationCount;
    constexpr uint32_t kFrameSize = 512;
    constexpr float kElevationStep = 25.0f * std::numbers::pi_v<float> / 180.0f;
    constexpr int kGridSize = 60;

    const char* const kImpostorSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct ImpostorData
{
    float4 right;          // the camera's right axis
    float4 up;             // the camera's up axis
    float4 viewPosition;   // the camera's position
    float4 centerRadius;   // xyz: the bounding sphere's centre over the statue's origin, w: its radius
    float4 frameInfo;      // x: azimuths, y: elevations, z: the elevation step in radians
};
VT_DECLARE_MATERIAL_UNIFORMS(ImpostorData, impostor)
VT_DECLARE_baseColorMap_AS(Sampler2DArray, impostorMap)

// The instance matrix's columns: Metal 6-9, Vulkan 5-8, as the forward program reads them.
#if defined(VT_TARGET_METAL)
#define IMPOSTOR_INSTANCE0 6
#define IMPOSTOR_INSTANCE1 7
#define IMPOSTOR_INSTANCE2 8
#define IMPOSTOR_INSTANCE3 9
#else
#define IMPOSTOR_INSTANCE0 5
#define IMPOSTOR_INSTANCE1 6
#define IMPOSTOR_INSTANCE2 7
#define IMPOSTOR_INSTANCE3 8
#endif

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
    [[vk::location(IMPOSTOR_INSTANCE0)]] float4 instance0 : INSTANCE0;
    [[vk::location(IMPOSTOR_INSTANCE1)]] float4 instance1 : INSTANCE1;
    [[vk::location(IMPOSTOR_INSTANCE2)]] float4 instance2 : INSTANCE2;
    [[vk::location(IMPOSTOR_INSTANCE3)]] float4 instance3 : INSTANCE3;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 uv0 : TEXCOORD0;
    float layer : TEXCOORD1;
};

[shader("vertex")]
Varyings impostorVertex(VertexIn input)
{
    // The statue's ground position and scale, from the instance matrix.
    const float scale = length(input.instance0.xyz);
    const float3 center = input.instance3.xyz + impostor.centerRadius.xyz * scale;

    // The baked view closest to the direction from the statue to the camera.
    const float3 toCamera = normalize(impostor.viewPosition.xyz - center);
    const float azimuth = atan2(toCamera.x, toCamera.z);
    const float elevation = asin(clamp(toCamera.y, -1.0, 1.0));
    const float azimuthCount = impostor.frameInfo.x;
    const float azimuthSlot = floor(azimuth / (6.28318530718 / azimuthCount) + 0.5);
    const float azimuthIndex = azimuthSlot - azimuthCount * floor(azimuthSlot / azimuthCount);
    const float elevationIndex = clamp(floor(elevation / impostor.frameInfo.z + 0.5), 0.0, impostor.frameInfo.y - 1.0);

    // A quad facing the camera, covering the bounding sphere.
    const float size = 2.0 * impostor.centerRadius.w * scale;
    const float3 position = center + (impostor.right.xyz * input.position.x + impostor.up.xyz * input.position.y) * size;

    Varyings output;
    output.uv0 = input.uv0;
    output.layer = elevationIndex * azimuthCount + azimuthIndex;
    float4 clip = mul(float4(position, 1.0), drawViewProjection());
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    return output;
}

[shader("fragment")]
float4 impostorFragment(Varyings input) : SV_Target0
{
    const float4 color = impostorMap.Sample(float3(input.uv0, floor(input.layer + 0.5)));
    if (color.a < 0.5) {
        discard;
    }
    // The mips blend the statue with the transparent black around it: un-premultiplied, the
    // distant impostors keep their colour instead of darkening at the edges.
    return float4(color.rgb / color.a, 1.0);
}
)slang";

    class ImpostorMaterial final : public ShaderMaterial
    {
    public:
        using ShaderMaterial::ShaderMaterial;

        void setView(const Vector3& right, const Vector3& up, const Vector3& position)
        {
            right.store(_data.right);
            up.store(_data.up);
            position.store(_data.viewPosition);
            markUniformsDirty();
        }

        void setStatue(const Vector3& centerOffset, const float radius)
        {
            centerOffset.store(_data.centerRadius);
            _data.centerRadius[3] = radius;
            _data.frameInfo[0] = static_cast<float>(kAzimuthCount);
            _data.frameInfo[1] = static_cast<float>(kElevationCount);
            _data.frameInfo[2] = kElevationStep;
            markUniformsDirty();
        }

        const void* customUniformData(size_t& outSize) const override
        {
            outSize = sizeof(_data);
            return &_data;
        }

    private:
        struct alignas(16) Data
        {
            float right[4] = {};
            float up[4] = {};
            float viewPosition[4] = {};
            float centerRadius[4] = {};
            float frameInfo[4] = {};
        } _data;
    };

    BoundingBox renderBounds(Entity* entity)
    {
        BoundingBox bounds;
        bool first = true;
        for (auto* render : entity->findComponents<RenderComponent>()) {
            for (MeshInstance* meshInstance : render->meshInstances()) {
                const BoundingBox aabb = meshInstance->aabb();
                if (first) {
                    bounds = aabb;
                    first = false;
                } else {
                    bounds.add(aabb);
                }
            }
        }
        return bounds;
    }
}

class TextureArrayImpostorsExample final : public ExampleApp
{
public:
    TextureArrayImpostorsExample() : ExampleApp({.title = "Texture Array Impostors"}) {}

protected:
    bool create() override
    {
        _statueAsset = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        auto* statueContainer = _statueAsset->resourceAs<ContainerResource>();
        if (!statueContainer) {
            spdlog::error("texture-array-impostors needs models/statue.glb");
            return false;
        }

        // Image based lighting and a skybox
        if (Texture* helipad = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(helipad);
        }
        scene()->setSkyboxMip(1);
        scene()->setSkyboxIntensity(0.7f);

        // A layer only the baking cameras render
        _bakeLayer = std::make_shared<Layer>("ImpostorBake", LAYERID_IMPOSTOR_BAKE);
        scene()->layers()->pushOpaque(_bakeLayer);
        scene()->layers()->pushTransparent(_bakeLayer);

        // A directional light, lighting both the baked statue and the world
        auto* light = new Entity();
        light->setEngine(engine());
        light->setName("light");
        if (auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            component->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            component->setColor(Color(1.0f, 0.95f, 0.85f, 1.0f));
            component->setIntensity(1.5f);
            component->setLayers({LAYERID_WORLD, LAYERID_IMPOSTOR_BAKE});
        }
        light->setLocalEulerAngles(50.0f, 30.0f, 0.0f);
        root()->addChild(light);

        // The statue to bake, rendered by the baking cameras only
        _bakeStatue = statueContainer->instantiateRenderEntity();
        for (auto* render : _bakeStatue->findComponents<RenderComponent>()) {
            render->setLayers({LAYERID_IMPOSTOR_BAKE});
        }
        root()->addChild(_bakeStatue);

        // The bounding sphere of the statue, which each baked view frames
        const BoundingBox aabb = renderBounds(_bakeStatue);
        const Vector3 center = aabb.center();
        const float radius = aabb.halfExtents().length();

        // A texture array with a layer for each baked view
        TextureOptions textureOptions;
        textureOptions.name = "ImpostorTexture";
        textureOptions.width = kFrameSize;
        textureOptions.height = kFrameSize;
        textureOptions.arrayLength = kFrameCount;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = true;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        _impostorTexture = std::make_shared<Texture>(device().get(), textureOptions);
        _impostorTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _impostorTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        // An orthographic camera per baked view, each rendering to its own layer of the array
        for (int elevationIndex = 0; elevationIndex < kElevationCount; ++elevationIndex) {
            for (int azimuthIndex = 0; azimuthIndex < kAzimuthCount; ++azimuthIndex) {
                const int layer = elevationIndex * kAzimuthCount + azimuthIndex;

                RenderTargetOptions targetOptions;
                targetOptions.graphicsDevice = device().get();
                targetOptions.colorBuffer = _impostorTexture.get();
                targetOptions.face = layer;
                targetOptions.depth = true;
                targetOptions.samples = 4;
                targetOptions.name = "ImpostorLayer" + std::to_string(layer);
                const auto renderTarget = device()->createRenderTarget(targetOptions);

                const float azimuth = static_cast<float>(azimuthIndex) / kAzimuthCount * 2.0f * std::numbers::pi_v<float>;
                const float elevation = static_cast<float>(elevationIndex) * kElevationStep;
                const Vector3 direction(std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
                    std::cos(elevation) * std::cos(azimuth));

                auto* bakeCamera = new Entity();
                bakeCamera->setEngine(engine());
                bakeCamera->setName("ImpostorCamera" + std::to_string(layer));
                auto* camera = static_cast<CameraComponent*>(bakeCamera->addComponent<CameraComponent>());
                camera->setLayers({LAYERID_IMPOSTOR_BAKE});
                camera->camera()->setRenderTarget(renderTarget);
                camera->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 0.0f));
                camera->camera()->setProjection(ProjectionType::Orthographic);
                camera->camera()->setOrthoHeight(radius);
                camera->camera()->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
                camera->camera()->setAspectRatio(1.0f);
                camera->camera()->setNearClip(radius);
                camera->camera()->setFarClip(radius * 5.0f);
                camera->setToneMapping(TONEMAP_ACES);
                // render in order, before the main camera
                camera->setPriority(layer - kFrameCount);
                root()->addChild(bakeCamera);
                bakeCamera->setPosition(center + direction * (radius * 3.0f));
                bakeCamera->lookAt(center);
                _bakeCameras.push_back(bakeCamera);
            }
        }

        // A full statue in the middle of the field, at twice the size, for comparison
        _statue = statueContainer->instantiateRenderEntity();
        _statue->setLocalScale(2.0f, 2.0f, 2.0f);
        root()->addChild(_statue);

        // The ground
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(0.35f, 0.33f, 0.3f, 1.0f));
        _groundMaterial->setGloss(0.2f);
        createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f), Vector3(3000.0f, 1.0f, 3000.0f));

        // A quad with corners at -0.5 .. 0.5, which the shader turns to face the camera
        PrimitiveGeometry quad;
        quad.positions = {-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.5f, 0.5f, 0.0f, -0.5f, 0.5f, 0.0f};
        quad.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
        quad.uvs = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
        quad.indices = {0, 1, 2, 0, 2, 3};
        _quad = createMeshFromGeometry(device(), quad);

        auto shader = getOrCreateSlangShaderFromSource(device().get(), "texture-array-impostors", kImpostorSlang);
        if (!shader) {
            spdlog::error("texture-array-impostors: the shader did not compile");
            return false;
        }
        _material = std::make_shared<ImpostorMaterial>("ImpostorShader", shader);
        _material->setCullMode(CullMode::CULLFACE_NONE);
        _material->setBaseColorTexture(_impostorTexture.get());
        _material->setStatue(center, radius);

        // The statues on a jittered grid, leaving the middle for the full statue
        const float spacing = radius * 2.5f;
        std::mt19937 random(9);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<float> matrices;
        for (int z = 0; z < kGridSize; ++z) {
            for (int x = 0; x < kGridSize; ++x) {
                const float px = (static_cast<float>(x) - (kGridSize - 1) * 0.5f + (unit(random) - 0.5f) * 0.6f) * spacing;
                const float pz = (static_cast<float>(z) - (kGridSize - 1) * 0.5f + (unit(random) - 0.5f) * 0.6f) * spacing;
                const float scale = 0.7f + unit(random) * 0.6f;
                if (std::abs(px) < spacing * 2.0f && std::abs(pz) < spacing * 2.0f) {
                    continue;
                }
                const float m[16] = {scale, 0.0f, 0.0f, 0.0f, 0.0f, scale, 0.0f, 0.0f, 0.0f, 0.0f, scale, 0.0f,
                    px, 0.0f, pz, 1.0f};
                matrices.insert(matrices.end(), m, m + 16);
            }
        }
        const int instanceCount = static_cast<int>(matrices.size() / 16);
        VertexBufferOptions bufferOptions;
        bufferOptions.data.resize(matrices.size() * sizeof(float));
        std::memcpy(bufferOptions.data.data(), matrices.data(), bufferOptions.data.size());
        _instanceBuffer = device()->createVertexBuffer(VertexFormat::defaultInstancingFormat(), instanceCount,
            bufferOptions);

        auto* impostors = new Entity();
        impostors->setEngine(engine());
        impostors->setName("impostors");
        if (auto* render = static_cast<RenderComponent*>(impostors->addComponent<RenderComponent>())) {
            auto meshInstance = std::make_unique<MeshInstance>(_quad, std::static_pointer_cast<Material>(_material),
                impostors);
            meshInstance->setInstancing(_instanceBuffer, instanceCount);
            // the instances cover the whole field, far outside the bounds of the single quad
            meshInstance->setCull(false);
            render->addMeshInstance(std::move(meshInstance));
        }
        root()->addChild(impostors);

        // The camera, far enough out to see the field, looking slightly down at the full statue
        _cameraEntity = createCamera(Vector3(0.0f));
        auto* cameraComponent = _cameraEntity->findComponent<CameraComponent>();
        cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        cameraComponent->setToneMapping(TONEMAP_ACES);
        cameraComponent->camera()->setFarClip(5000.0f);
        const float startDistance = radius * 11.0f;
        const float startPitch = 14.0f * std::numbers::pi_v<float> / 180.0f;
        _cameraEntity->setPosition(Vector3(center.getX() * 2.0f, center.getY() * 2.0f + startDistance * std::sin(startPitch),
            center.getZ() * 2.0f + startDistance * std::cos(startPitch)));
        if (auto* controls = addOrbitControls(_cameraEntity, renderBounds(_statue).center())) {
            controls->setPitchRange(Vector2(-85.0f, 0.0f));
            controls->setZoomRange(Vector2(0.01f, spacing * kGridSize));
            controls->setEnableFly(false);
            controls->storeResetState();
        }
        return true;
    }

    void update(float /*dt*/) override
    {
        if (_cameraEntity && _material) {
            const Matrix4& world = _cameraEntity->worldTransform();
            _material->setView(Vector3(world.getColumn(0)).normalized(), Vector3(world.getColumn(1)).normalized(),
                Vector3(world.getColumn(3)));
        }
    }

    void postRender() override
    {
        // The array is baked during the first frame, after which the baking cameras, their render
        // targets and the statue are no longer needed. The array stays.
        if (_baked) {
            return;
        }
        _baked = true;
        for (Entity* bakeCamera : _bakeCameras) {
            auto owned = bakeCamera->remove();
        }
        _bakeCameras.clear();
        if (_bakeStatue) {
            auto owned = _bakeStatue->remove();
            _bakeStatue = nullptr;
        }
    }

private:
    std::unique_ptr<Asset> _statueAsset;
    std::unique_ptr<Asset> _helipad;
    std::shared_ptr<Layer> _bakeLayer;
    Entity* _bakeStatue = nullptr;
    Entity* _statue = nullptr;
    std::vector<Entity*> _bakeCameras;
    std::shared_ptr<Texture> _impostorTexture;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<Mesh> _quad;
    std::shared_ptr<ImpostorMaterial> _material;
    std::shared_ptr<VertexBuffer> _instanceBuffer;
    Entity* _cameraEntity = nullptr;
    bool _baked = false;
};

VISUTWIN_EXAMPLE_MAIN(TextureArrayImpostorsExample)
