// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream shaders/ground-fog.
//
// A layer of soft white ground fog drifting over the low-poly terrain (scaled 30x). The fog is a
// 20 x 20 segment plane of 500 x 500 at height 25, drawn blended without depth writes by a
// ShaderMaterial: its vertex stage lifts each vertex by up to 25 units by the clouds texture,
// read at a slowly scrolling uv, and its fragment stage averages three samples of the same
// texture scrolling in different directions and speeds into an opacity, which it fades out where
// the fog comes close to the terrain behind it: the difference between the fog's linear depth and
// the scene's, both over the far clip, times a softening of 50 (or 1000 with the softness off),
// through a smoothstep. The scene depth comes from the camera's depth map, copied at the depth
// layer before the fog draws, which is also shown, linearised over the far clip, in the
// bottom-right quarter of the window. The helipad environment (mip 3, turned -70 degrees) lights
// the terrain but the sky is not drawn: the camera clears to (150, 213, 63) / 255, with a far clip
// of 1000 and ACES tone mapping. A directional light at euler (45, 350, 20) casts PCF3 shadows
// (resolution 2048, distance 1000, bias 0.3, normal offset 0.2). The camera orbits the tree
// "Arbol 2.002" from 320 units away, in the direction of its authored position (-200, 120, 225).
//
// Keys stand in for upstream's controls panel:
//   S  softness on / off (on to begin with)
//
// DEVIATIONS:
// - The shaders are Slang (one source for both backends): the fog is a ShaderMaterial over
//   getOrCreateSlangShaderFromSource, the clouds texture bound twice, as the material's base
//   colour map for the fragment stage and as its displacement map, the one vertex-stage texture
//   slot. The time, the softening and the camera's clip planes travel in the material's own
//   uniform block, and the fog's linear depth is its clip w, which is the view depth for a
//   perspective camera.
// - The depth preview is a quad pass of its own appended to the frame, with the viewport upstream's
//   texture renderer draws its quad into (0.725, 0.725, 0.25, 0.25 of the window).
// - Orbit camera: upstream's orbit-camera script frames the tree, then sets distance 320 on the
//   first frame (inertia 0.2, distanceMax 600). This port starts CameraControls at that pose and
//   clamps zoom to 600; CameraControls has no inertia factor.
//
// @credit Terrain Low Poly by Sketchfab, CC BY 4.0
//
#include <algorithm>
#include <memory>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/shaderBindings.h"
#include "platform/graphics/texture.h"
#include "platform/input/keyboard.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/layer.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/renderer/forwardRenderer.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    // The fog: a plane lifted by the clouds texture, its opacity faded out near the scene behind it.
    const char* const kGroundFogSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct FogData
{
    float4 params;        // x: time, y: softening
    float4 cameraParams;  // x: 1 / far clip, y: far clip, z: near clip, w: 1 when orthographic
};
VT_DECLARE_MATERIAL_UNIFORMS(FogData, fog)
VT_DECLARE_baseColorMap      // the fog texture, for the fragment stage
VT_DECLARE_displacementMap   // the same texture, for the vertex stage
VT_DECLARE_materialSampler
VT_DECLARE_sceneDepthGrab
// nearest, no mips, clamp: the sampler the forward pass reads the depth copy through
[[vk::binding(13, 3)]] SamplerState nearestClampSampler : register(s8);

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(2)]] float2 uv0 : TEXCOORD0;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 texCoord0 : TEXCOORD0;
    float2 texCoord1 : TEXCOORD1;
    float2 texCoord2 : TEXCOORD2;
    float4 screenPos : TEXCOORD3;
    float depth : TEXCOORD4;
};

float linearizeDepth(float z)
{
    if (fog.cameraParams.w == 0.0) {
        return (fog.cameraParams.z * fog.cameraParams.y) /
            (fog.cameraParams.y + z * (fog.cameraParams.z - fog.cameraParams.y));
    }
    return fog.cameraParams.z + z * (fog.cameraParams.y - fog.cameraParams.z);
}

[shader("vertex")]
Varyings groundFogVertex(VertexIn input)
{
    Varyings output;
    const float time = fog.params.x;

    // 3 scrolling texture coordinates with different direction and speed
    output.texCoord0 = input.uv0 * 2.0 + float2(time * 0.003, time * 0.01);
    output.texCoord1 = input.uv0 * 1.5 + float2(time * -0.02, time * 0.02);
    output.texCoord2 = input.uv0 * 1.0 + float2(time * 0.01, time * -0.003);

    // sample the fog texture to have elevation for this vertex
    const float2 offsetTexCoord = input.uv0 + float2(time * 0.001, time * -0.0003);
    const float offset = displacementMap.SampleLevel(materialSampler, offsetTexCoord, 0.0).r;

    // vertex in the world space
    float4 pos = mul(float4(input.position, 1.0), drawModel());

    // move it up based on the offset
    pos.y += offset * 25.0;

    // position in projected (screen) space
    const float4 projPos = mul(pos, drawViewProjection());
    float4 clip = projPos;
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;

    // the linear depth of the vertex (in camera space): clip w is the view depth of a
    // perspective camera
    output.depth = projPos.w;

    // screen fragment position, used to sample the depth texture
    output.screenPos = projPos;
    return output;
}

[shader("fragment")]
float4 groundFogFragment(Varyings input) : SV_Target0
{
    // sample the texture 3 times and compute average intensity of the fog
    const float4 diffuseTexture0 = baseColorMap.Sample(input.texCoord0);
    const float4 diffuseTexture1 = baseColorMap.Sample(input.texCoord1);
    const float4 diffuseTexture2 = baseColorMap.Sample(input.texCoord2);
    float alpha = 0.5 * (diffuseTexture0.r + diffuseTexture1.r + diffuseTexture2.r);

    // convert the screen position to the depth texture's uv (its row 0 is the top of the frame)
    const float2 screenCoord = (input.screenPos.xy / input.screenPos.w) * float2(0.5, -0.5) + 0.5;

    // read the depth from the depth buffer
    const float rawDepth = sceneDepthGrab.SampleLevel(nearestClampSampler, screenCoord, 0.0).r;
    const float sceneDepth = linearizeDepth(rawDepth) * fog.cameraParams.x;

    // depth of the current fragment (on the fog plane)
    const float fragmentDepth = input.depth * fog.cameraParams.x;

    // difference between these two depths is used to adjust the alpha, to fade out
    // the fog near the geometry
    const float depthDiff = clamp(abs(fragmentDepth - sceneDepth) * fog.params.y, 0.0, 1.0);
    alpha *= smoothstep(0.0, 1.0, depthDiff);

    // final color
    const float3 fogColor = float3(1.0, 1.0, 1.0);
    return float4(fogColor, alpha);
}
)slang";

    // The depth preview: the camera's depth map, linearised over the far clip and gamma-encoded.
    const char* const kDepthPreviewSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct DepthPreviewUniforms
{
    float4 cameraParams;  // x: near clip, y: far clip, z: 1 when orthographic
};
VT_DECLARE_QUAD_UNIFORMS(DepthPreviewUniforms, u)
VT_DECLARE_QUAD_DEPTH_INPUT0(sceneDepthMap)

[shader("vertex")]
QuadVarying depthPreviewVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 depthPreviewFragment(QuadVarying input) : SV_Target0
{
    const float raw = sceneDepthMap.SampleLevel(input.uv, 0.0);
    const float depth = quadLinearDepth(raw, u.cameraParams.x, u.cameraParams.y, u.cameraParams.z > 0.5) /
        u.cameraParams.y;
    // gammaCorrectOutput
    return float4(pow(float3(depth, depth, depth) + 0.0000001, float3(1.0 / 2.2, 1.0 / 2.2, 1.0 / 2.2)), 1.0);
}
)slang";

    // Where the preview is drawn, as fractions of the window: left, top, width, height.
    constexpr float kPreviewX = 0.725f;
    constexpr float kPreviewY = 0.725f;
    constexpr float kPreviewW = 0.25f;
    constexpr float kPreviewH = 0.25f;

    class FogMaterial final : public ShaderMaterial
    {
    public:
        using ShaderMaterial::ShaderMaterial;

        void setTime(const float time)
        {
            // A block the material packs is reused until it says it changed.
            markUniformsDirty();
            _data.params[0] = time;
        }

        void setSoftening(const float softening)
        {
            markUniformsDirty();
            _data.params[1] = softening;
        }

        void setCamera(const Camera* camera)
        {
            markUniformsDirty();
            _data.cameraParams[0] = 1.0f / camera->farClip();
            _data.cameraParams[1] = camera->farClip();
            _data.cameraParams[2] = camera->nearClip();
            _data.cameraParams[3] = camera->projection() == ProjectionType::Orthographic ? 1.0f : 0.0f;
        }

        // The fog texture, read by the fragment stage at the base colour slot and by the vertex
        // stage at the displacement map slot.
        void setFogTexture(Texture* texture)
        {
            _fogTexture = texture;
            setBaseColorTexture(texture);
        }

        void getTextureSlots(std::vector<TextureSlot>& slots) const override
        {
            ShaderMaterial::getTextureSlots(slots);
            if (_fogTexture) {
                slots.push_back({kDisplacementMapMetalSlot, _fogTexture});
            }
        }

        const void* customUniformData(size_t& outSize) const override
        {
            outSize = sizeof(_data);
            return &_data;
        }

    private:
        struct alignas(16) Data
        {
            float params[4] = {0.0f, 50.0f, 0.0f, 0.0f};
            float cameraParams[4] = {0.001f, 1000.0f, 0.1f, 0.0f};
        } _data;
        Texture* _fogTexture = nullptr;
    };

    struct DepthPreviewUniforms
    {
        float cameraParams[4];
    };

    // Draws the camera's depth map into a rectangle of the back buffer, at the end of the frame.
    class RenderPassDepthPreview final : public RenderPassShaderQuad
    {
    public:
        RenderPassDepthPreview(const std::shared_ptr<GraphicsDevice>& device, const Camera* camera)
            : RenderPassShaderQuad(device), _camera(camera)
        {
        }

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("ground-fog-depth-preview", kDepthPreviewSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            const auto graphicsDevice = device();
            Texture* depth = graphicsDevice ? graphicsDevice->sceneDepthGrabMap() : nullptr;
            if (!shader() || !depth || !_camera) {
                return;
            }
            DepthPreviewUniforms uniforms{};
            uniforms.cameraParams[0] = _camera->nearClip();
            uniforms.cameraParams[1] = _camera->farClip();
            uniforms.cameraParams[2] = _camera->projection() == ProjectionType::Orthographic ? 1.0f : 0.0f;
            setQuadUniforms(uniforms);
            setQuadTextureBinding(0, depth);
            RenderPassShaderQuad::execute();
        }

    private:
        const Camera* _camera;
    };

    // The fog plane: a unit plane with 20 segments along each axis, its UVs running
    // as the 'plane' primitive's do (v = 0 at -Z).
    PrimitiveGeometry createSubdividedPlane(const int widthSegments, const int lengthSegments)
    {
        PrimitiveGeometry geometry;
        uint32_t vertex = 0;
        const auto lengthStride = static_cast<uint32_t>(lengthSegments);
        for (int i = 0; i <= widthSegments; ++i) {
            for (int j = 0; j <= lengthSegments; ++j) {
                const float u = static_cast<float>(i) / static_cast<float>(widthSegments);
                const float v = static_cast<float>(j) / static_cast<float>(lengthSegments);
                geometry.positions.insert(geometry.positions.end(), {-0.5f + u, 0.0f, 0.5f - v});
                geometry.normals.insert(geometry.normals.end(), {0.0f, 1.0f, 0.0f});
                geometry.uvs.insert(geometry.uvs.end(), {u, 1.0f - v});
                if (i < widthSegments && j < lengthSegments) {
                    geometry.indices.insert(geometry.indices.end(), {
                        vertex + lengthStride + 1, vertex + 1, vertex,
                        vertex + lengthStride + 1, vertex + lengthStride + 2, vertex + 1});
                }
                ++vertex;
            }
        }
        return geometry;
    }
}

class GroundFogExample final : public ExampleApp
{
public:
    GroundFogExample() : ExampleApp({.title = "Ground Fog"}) {}

protected:
    bool create() override
    {
        _terrainAsset = std::make_unique<Asset>("terrain", AssetType::CONTAINER, assetPath("models/terrain.glb"));
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _cloudsAsset = std::make_unique<Asset>("color", AssetType::TEXTURE, assetPath("textures/clouds.jpg"));
        auto* terrainContainer = _terrainAsset->resourceAs<ContainerResource>();
        Texture* envAtlas = _envAtlas->resourceAs<Texture>();
        Texture* clouds = _cloudsAsset->resourceAs<Texture>();
        if (!terrainContainer || !envAtlas || !clouds) {
            spdlog::error("ground-fog needs models/terrain.glb, the helipad env atlas and textures/clouds.jpg");
            return false;
        }

        // The skydome, which lights the scene but is not drawn
        scene()->setSkyboxMip(3);
        scene()->setEnvAtlas(envAtlas);
        scene()->setSkyboxRotation(Quaternion::fromEulerAngles(0.0f, -70.0f, 0.0f));
        if (const auto layers = scene()->layers()) {
            if (const auto skyLayer = layers->getLayerById(LAYERID_SKYBOX)) {
                skyLayer->setEnabled(false);
            }
        }

        // The terrain
        auto* terrain = terrainContainer->instantiateRenderEntity();
        terrain->setEngine(engine());
        terrain->setLocalScale(30.0f, 30.0f, 30.0f);
        root()->addChild(terrain);

        // A tree in the middle to focus on
        Vector3 focusPoint(0.0f, 0.0f, 0.0f);
        if (auto* tree = dynamic_cast<Entity*>(terrain->findByName("Arbol 2.002"))) {
            focusPoint = entityBounds(tree).center();
        }

        // The camera, looking at the tree from its authored position, moved to 320 units away
        const Vector3 authoredPosition(-200.0f, 120.0f, 225.0f);
        const Vector3 direction = (authoredPosition - focusPoint).normalized();
        auto* camera = createCamera(focusPoint + direction * 320.0f);
        _cameraComponent = camera->findComponent<CameraComponent>();
        if (!_cameraComponent || !_cameraComponent->camera()) {
            return false;
        }
        _cameraComponent->camera()->setClearColor(Color(150.0f / 255.0f, 213.0f / 255.0f, 63.0f / 255.0f, 1.0f));
        _cameraComponent->camera()->setFarClip(1000.0f);
        _cameraComponent->setToneMapping(TONEMAP_ACES);
        camera->lookAt(focusPoint);
        if (auto* controls = addOrbitControls(camera, focusPoint)) {
            controls->setZoomRange(Vector2(0.0f, 600.0f));
            controls->storeResetState();
        }

        // The camera renders the scene's depth map
        _cameraComponent->requestSceneDepthMap(true);

        // A directional light casting shadows
        auto* light = createDirectionalLight(Vector3(45.0f, 350.0f, 20.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComponent = light->findComponent<LightComponent>()) {
            lightComponent->setShadowBias(0.3f);
            lightComponent->setShadowNormalBias(0.2f);
            lightComponent->setShadowDistance(1000.0f);
            lightComponent->setShadowResolution(2048);
            lightComponent->setShadowType(SHADOW_PCF3_32F);
        }

        // The fog material: blended, without depth writes
        auto shader = getOrCreateSlangShaderFromSource(device().get(), "ground-fog", kGroundFogSlang);
        if (!shader) {
            spdlog::error("ground-fog: the fog shader did not compile");
            return false;
        }
        _material = std::make_shared<FogMaterial>("GroundFogShader", shader);
        _material->setFogTexture(clouds);
        _material->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        _material->setDepthState(std::make_shared<DepthState>(DepthState::noWrite()));
        _material->setTransparent(true);

        // A subdivided plane, so the shader can move its vertices
        _planeMesh = createMeshFromGeometry(device(), createSubdividedPlane(20, 20));
        auto* ground = new Entity();
        ground->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>())) {
            auto meshInstance = std::make_unique<MeshInstance>(_planeMesh.get(), _material.get(), ground);
            meshInstance->setCastShadow(false);
            meshInstance->setReceiveShadow(false);
            render->addMeshInstance(std::move(meshInstance));
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }
        ground->setLocalScale(500.0f, 1.0f, 500.0f);
        ground->setLocalPosition(0.0f, 25.0f, 0.0f);
        root()->addChild(ground);

        // The depth map, shown in the corner
        _previewPass = std::make_shared<RenderPassDepthPreview>(device(), _cameraComponent->camera());
        _previewPass->init(nullptr);
        engine()->renderer()->addAppendPass(_previewPass);

        spdlog::info("Keys: S softness");
        return true;
    }

    void update(const float dt) override
    {
        if (const auto* keyboard = engine()->keyboard(); keyboard && keyboard->wasPressed(Key::S)) {
            _softness = !_softness;
        }

        // The time, the softening and the camera's planes for the shader
        _time += dt;
        _material->setTime(_time);
        _material->setSoftening(_softness ? 50.0f : 1000.0f);
        _material->setCamera(_cameraComponent->camera());

        // The preview follows the window size
        const auto [width, height] = device()->size();
        const auto screenW = static_cast<float>(std::max(1, width));
        const auto screenH = static_cast<float>(std::max(1, height));
        const Vector4 viewport(kPreviewX * screenW, kPreviewY * screenH, kPreviewW * screenW, kPreviewH * screenH);
        _previewPass->setViewport(viewport);
        _previewPass->setScissor(viewport);
    }

    void destroy() override
    {
        if (_material) {
            _material->setFogTexture(nullptr);
        }
    }

private:
    std::unique_ptr<Asset> _terrainAsset;
    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _cloudsAsset;
    std::shared_ptr<FogMaterial> _material;
    std::shared_ptr<Mesh> _planeMesh;
    std::shared_ptr<RenderPassDepthPreview> _previewPass;
    CameraComponent* _cameraComponent = nullptr;
    bool _softness = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(GroundFogExample)
