// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream shaders/grab-pass.
//
// A frosted glass shader refracting the scene colour map, which the Depth layer grabs with
// mipmaps: the rougher the glass, the more blurred the mip level it samples. The scene stands
// in the Wide Street HDRI (Poly Haven, CC BY 4.0), a full resolution skybox projected onto a dome
// with a flat ground (scale 200, centre (0, 0.05, 0)) and the environment lighting generated from
// the same image, at exposure 0.7. A sun (50, 30, 0) of intensity 1.5 casts soft VSM shadows
// (distance 60, intensity 0.7, 2048) of the shapes onto a shadow catcher of 60 on the ground.
// Seven shapes (sphere, torus, cone, capsule, cylinder, box, torus; the tori stand up as rings,
// the capsule lies down) of scale 4 ring the glass on a circle of radius 9, each resting on the
// ground, with gloss 0.6 and metalness 0.4; those behind the glass as seen from the camera rise
// by up to 7 (a smoothstep from 0.3 to 0.9 of how far behind they are), to be seen refracted
// through it, and spin at 20 degrees a second. The depth layer, where the camera copies the frame
// into its scene colour map, is moved after the world's opaque sublayer and the skybox, so the
// copy holds both. The glass, a rounded box (radius 0.12, 8 segments) of scale 7 at (0, 9.5, 0)
// that tumbles at (-6, 6, -9) degrees a second, is drawn after the copy with a ShaderMaterial
// (blended, in the transparent sublayer): it reads the copy at its own screen position, offset by
// its view-space normal (the rounded edges bend the view) and by a normal map scaled by the
// roughness (1 minus the red of a grey roughness texture), at a mip level 7 x the roughness, and
// brightens the frosted parts (roughness x 0.06) and the edges (edge^2 x 0.3). The camera orbits
// at radius 27 and height 7, 0.2 radians a second, looking at (0, 7.5, 0), with ACES tone mapping
// and a far clip of 500.
//
// The mipmaps of the scene colour map are generated with the Gaussian filter, which blurs the
// frosted glass smoothly, or with the box filter, whose blocks flicker as the scene moves:
//   G    toggle the Gaussian mipmaps (upstream's control; VISUTWIN_MIPMAP_FILTER=box starts
//        with the box filter)
//
// DEVIATION: the shader is Slang (one source for both backends), a ShaderMaterial over
// getOrCreateSlangShaderFromSource: the offset map is the material's base colour map, the
// roughness map its normal map slot, and the view matrix and the screen size travel in the
// material's own uniform block. The copy is read with this engine's top-left screen origin, as
// upstream's WebGPU shader reads it, so the normal's vertical offset is flipped as that shader
// flips it.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <string_view>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/script/shadowCatcher.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/envLighting.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    // The glass: the scene colour copy, offset by the glass's normal and bumps, blurred by its
    // roughness.
    const char* const kGlassSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct GlassData
{
    float4 view0;        // the camera's view matrix, by columns
    float4 view1;
    float4 view2;
    float4 view3;
    float4 screenSize;   // xy: the size of the target in pixels, zw: its inverse
};
VT_DECLARE_MATERIAL_UNIFORMS(GlassData, glass)
VT_DECLARE_baseColorMap   // the bumps of the glass (a normal map)
VT_DECLARE_normalMap      // its roughness, frosted around a clear logo
VT_DECLARE_sceneColorGrab
// linear, linear mips, clamp: the sampler the forward pass reads the copy through
[[vk::binding(12, 3)]] SamplerState linearClampSampler : register(s7);

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
    float3 viewNormal : TEXCOORD1;
};

[shader("vertex")]
Varyings glassVertex(VertexIn input)
{
    Varyings output;
    const float4 pos = mul(float4(input.position, 1.0), drawModel());
    float4 clip = mul(pos, drawViewProjection());
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    output.texCoord = input.uv0;

    // the normal in the view space, which bends the view through the glass
    const float4x4 view = float4x4(glass.view0, glass.view1, glass.view2, glass.view3);
    const float3 worldNormal = mul(input.normal, drawNormalMatrix());
    output.viewNormal = mul(float4(worldNormal, 0.0), view).xyz;
    return output;
}

[shader("fragment")]
float4 glassFragment(Varyings input) : SV_Target0
{
    const float roughness = 1.0 - normalMap.Sample(input.texCoord).r;

    // the view through the glass bends at its rounded edges, and on its bumps; the screen
    // position runs down, so the vertical direction of the normal is flipped
    const float3 normal = normalize(input.viewNormal);
    const float2 bumps = baseColorMap.Sample(input.texCoord).rg * 2.0 - 1.0;
    const float2 offset = float2(-normal.x, normal.y) * 0.08 + bumps * (0.2 + roughness) * 0.015;

    // the rougher the glass, the smaller and more blurred the mip level the scene is sampled from
    const float2 grabUv = input.position.xy * glass.screenSize.zw;
    const float3 color = sceneColorGrab.SampleLevel(linearClampSampler, grabUv + offset, roughness * 7.0).rgb;

    // the frosted glass and its edges are a little brighter
    const float edge = 1.0 - abs(normal.z);
    return float4(color + roughness * 0.06 + edge * edge * 0.3, 1.0);
}
)slang";

    class GlassMaterial final : public ShaderMaterial
    {
    public:
        using ShaderMaterial::ShaderMaterial;

        void setView(const Matrix4& view)
        {
            markUniformsDirty();
            for (int i = 0; i < 4; ++i) {
                view.getColumn(i).store(_data.view[i]);
            }
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
            float view[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
            float screenSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        } _data;
    };

    float smoothstep(const float edge0, const float edge1, const float x)
    {
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    // A ring of shapes around the glass: those behind it rise to its height, to be seen
    // refracted through it, and those in front of it rest on the ground, clear of the view of
    // the glass.
    struct ShapeDesc
    {
        const char* type;
        Color color;
    };
    const std::array<ShapeDesc, 7> kShapes = {{
        {"sphere", Color(1.0f, 0.42f, 0.42f, 1.0f)},     // #ff6b6b
        {"torus", Color(0.996f, 0.792f, 0.341f, 1.0f)},  // #feca57
        {"cone", Color(0.114f, 0.82f, 0.631f, 1.0f)},    // #1dd1a1
        {"capsule", Color(0.282f, 0.859f, 0.984f, 1.0f)},// #48dbfb
        {"cylinder", Color(0.549f, 0.361f, 0.941f, 1.0f)},// #8c5cf0
        {"box", Color(1.0f, 0.624f, 0.953f, 1.0f)},      // #ff9ff3
        {"torus", Color(0.329f, 0.627f, 1.0f, 1.0f)}     // #54a0ff
    }};
    constexpr float kShapeLift = 7.0f;
    constexpr float kCameraRadius = 27.0f;
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
        _hdri = std::make_unique<Asset>("hdri", AssetType::TEXTURE, assetPath("hdri/wide-street.hdr"),
            AssetData{.mipmaps = false});
        Texture* normal = _normal->resourceAs<Texture>();
        Texture* roughness = _roughness->resourceAs<Texture>();
        Texture* hdri = _hdri->resourceAs<Texture>();
        if (!normal || !roughness || !hdri) {
            spdlog::error("grab-pass needs textures/normal-map.png, textures/pc-gray.png and hdri/wide-street.hdr");
            return false;
        }

        // The street the scene stands in: a full resolution skybox projected onto a dome with a
        // flat ground, and the environment lighting generated from the same HDRI
        _skybox.reset(EnvLighting::generateSkyboxCubemap(device().get(), hdri));
        _envAtlas.reset(EnvLighting::generateAtlas(device().get(), hdri));
        scene()->setSkybox(_skybox.get());
        scene()->setEnvAtlas(_envAtlas.get());
        scene()->setSkyType(SKYTYPE_DOME);
        scene()->sky()->node()->setLocalScale(Vector3(200.0f, 200.0f, 200.0f));
        scene()->sky()->setCenter(Vector3(0.0f, 0.05f, 0.0f));
        scene()->setExposure(0.7f);

        // The sun casts soft shadows of the shapes, onto a shadow catcher on the ground
        Entity* sun = createDirectionalLight(Vector3(50.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f, true);
        sun->setName("Sun");
        if (auto* light = sun->findComponent<LightComponent>()) {
            light->setShadowDistance(60.0f);
            light->setShadowIntensity(0.7f);
            light->setShadowResolution(2048);
            light->setShadowType(SHADOW_VSM_16F);
        }

        auto* shadowCatcher = new Entity();
        shadowCatcher->setEngine(engine());
        shadowCatcher->setName("ShadowCatcher");
        shadowCatcher->addComponent<ScriptComponent>();
        shadowCatcher->script()->create<ShadowCatcher>()->setPlaneScale(60.0f);
        root()->addChild(shadowCatcher);

        // The depth layer is where the frame is copied to a texture for the following layers.
        // Move it after the world's opaque sublayer and the skybox, so the copy holds both.
        if (const auto layers = scene()->layers()) {
            if (const auto depthLayer = layers->getLayerById(LAYERID_DEPTH)) {
                layers->remove(depthLayer);
                layers->insertOpaque(depthLayer, 2);
            }
        }

        // A ring of shapes around the glass
        for (size_t index = 0; index < kShapes.size(); ++index) {
            const ShapeDesc& desc = kShapes[index];
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(desc.color);
            material->setGloss(0.6f);
            material->setMetalness(0.4f);
            material->setUseMetalness(true);
            _materials.push_back(material);

            const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(index) /
                static_cast<float>(kShapes.size());
            Entity* shape = createPrimitive(desc.type, material.get(),
                Vector3(9.0f * std::sin(angle), 0.0f, 9.0f * std::cos(angle)), Vector3(4.0f, 4.0f, 4.0f));
            shape->setName(desc.type);

            // the tori stand up as rings, and the capsule lies down
            if (std::string_view(desc.type) == "torus") {
                shape->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
            } else if (std::string_view(desc.type) == "capsule") {
                shape->setLocalEulerAngles(0.0f, 0.0f, 90.0f);
            }

            // the height at which the shape rests on the ground
            const BoundingBox bounds = entityBounds(shape);
            const float restHeight = -(bounds.center().getY() - bounds.halfExtents().getY());
            const Vector3& position = shape->localPosition();
            shape->setLocalPosition(position.getX(), restHeight, position.getZ());
            _shapes.push_back({shape, restHeight});
        }

        // The camera, which renders the scene and copies its colour for the glass
        _camera = createCamera(Vector3(0.0f, 7.0f, kCameraRadius));
        _camera->setName("SceneCamera");
        _cameraComp = _camera->findComponent<CameraComponent>();
        _cameraComp->camera()->setFarClip(500.0f);
        _cameraComp->setToneMapping(TONEMAP_ACES);
        _cameraComp->requestSceneColorMap(true);

        // The glass is a rounded cube, frosted around a clear logo. Its shader samples the scene
        // colour map, blurred more on the rougher parts of the glass.
        auto shader = getOrCreateSlangShaderFromSource(device().get(), "grab-pass-glass", kGlassSlang);
        if (!shader) {
            spdlog::error("grab-pass: the glass shader did not compile");
            return false;
        }
        _glassMaterial = std::make_shared<GlassMaterial>("GlassShader", shader);
        _glassMaterial->setBaseColorTexture(normal);
        _glassMaterial->setNormalTexture(roughness);
        // Transparent, so it renders after the depth layer captured the scene behind it
        _glassMaterial->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        _glassMaterial->setTransparent(true);

        RoundedBoxGeometryOptions geometry;
        geometry.radius = 0.12f;
        geometry.radiusSegments = 8;
        _glassMesh = createMeshFromGeometry(device(), createRoundedBoxGeometry(geometry));
        _glass = new Entity();
        _glass->setEngine(engine());
        _glass->setName("Glass");
        auto* render = static_cast<RenderComponent*>(_glass->addComponent<RenderComponent>());
        render->setCastShadows(false);
        render->addMeshInstance(std::make_unique<MeshInstance>(_glassMesh, _glassMaterial, _glass));
        _glass->setLocalPosition(0.0f, 9.5f, 0.0f);
        _glass->setLocalScale(Vector3(7.0f, 7.0f, 7.0f));
        root()->addChild(_glass);

        // VISUTWIN_MIPMAP_FILTER=box starts with the box filter, for a capture of either.
        if (const char* filter = std::getenv("VISUTWIN_MIPMAP_FILTER")) {
            _gaussian = std::string_view(filter) != "box";
        }
        applyMipmapFilter();
        spdlog::info("grab-pass: G toggles the Gaussian mipmaps of the scene colour map");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_G) {
            _gaussian = !_gaussian;
            applyMipmapFilter();
            return true;
        }
        return false;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Orbit the camera
        const float cameraX = kCameraRadius * std::sin(_time * 0.2f);
        const float cameraZ = kCameraRadius * std::cos(_time * 0.2f);
        _camera->setLocalPosition(cameraX, 7.0f, cameraZ);
        _camera->lookAt(Vector3(0.0f, 7.5f, 0.0f));

        // Spin the shapes, lifting them the further behind the glass they are from the camera
        for (auto& [shape, restHeight] : _shapes) {
            const Vector3 position = shape->localPosition();
            const float x = position.getX();
            const float z = position.getZ();
            const float behind = -(x * cameraX + z * cameraZ) / (std::hypot(x, z) * kCameraRadius);
            const float lift = kShapeLift * smoothstep(0.3f, 0.9f, behind);
            shape->setLocalPosition(x, restHeight + lift, z);
            shape->rotate(0.0f, 20.0f * dt, 0.0f);
        }

        // Tumble the glass
        _glass->rotate(-6.0f * dt, 6.0f * dt, -9.0f * dt);

        const auto [width, height] = device()->size();
        _glassMaterial->setScreenSize(static_cast<float>(width), static_cast<float>(height));
        _glassMaterial->setView(_camera->worldTransform().inverse());
    }

    void destroy() override
    {
        if (_glassMaterial) {
            _glassMaterial->setBaseColorTexture(nullptr);
            _glassMaterial->setNormalTexture(nullptr);
        }
    }

private:
    // The mipmaps of the scene colour map: the Gaussian filter blurs the frosted glass smoothly,
    // the box filter's blocks flicker as the scene moves.
    void applyMipmapFilter()
    {
        _cameraComp->setSceneColorMapMipmapFilter(_gaussian ? MipmapFilter::MIPMAPFILTER_GAUSSIAN
                                                            : MipmapFilter::MIPMAPFILTER_BOX);
        spdlog::info("grab-pass: {} mipmaps", _gaussian ? "Gaussian" : "box");
    }

    struct Shape
    {
        Entity* entity;
        float restHeight;
    };

    std::unique_ptr<Asset> _normal;
    std::unique_ptr<Asset> _roughness;
    std::unique_ptr<Asset> _hdri;
    std::unique_ptr<Texture> _skybox;
    std::unique_ptr<Texture> _envAtlas;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<GlassMaterial> _glassMaterial;
    std::shared_ptr<Mesh> _glassMesh;
    std::vector<Shape> _shapes;
    Entity* _glass = nullptr;
    Entity* _camera = nullptr;
    CameraComponent* _cameraComp = nullptr;
    bool _gaussian = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(GrabPassExample)
