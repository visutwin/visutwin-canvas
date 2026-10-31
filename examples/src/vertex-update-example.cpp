// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream compute/vertex-update.
//
// A compute shader rewrites the vertex buffer of a sphere every frame: each vertex is placed at
// its normal scaled per axis by 1 + sin(t + 10 y) * 0.1, 1 + cos(t + 5 x) * 0.1 and
// 1 + sin(t + 2 z) * 0.2, so the sphere wobbles while its normals stay those of the sphere. The
// sphere (radius 1, 100 latitude and longitude bands) wears the seaside rocks colour, normal and
// gloss maps, and switches between solid and wireframe every two seconds. The table mountain
// environment at skybox mip 2 and exposure 2; the camera at (0, 0, 3.92) clears to
// (0.4, 0.45, 0.5) and orbits the origin under the mouse. The performance HUD shows the frame's
// compute dispatches and passes.
//
// DEVIATIONS:
// - The vertex buffer is the engine's interleaved packed layout, so the kernel steps through it
//   by the vertex stride, where upstream asks for a separate (non-interleaved) storage layout.
// - The kernel is Slang: the buffer at index 0 and the uniform block after it (compute.h).
// - The deformed sphere reaches 1.2 from its centre, past the rest-pose bounds the culling reads,
//   so the mesh instance carries bounds of its own.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    // Moves every vertex of the buffer to its normal scaled by a time-varying strength.
    constexpr const char* kVertexUpdateSlang = R"slang(
struct VertexUpdateParams
{
    uint4 layout;   // x: vertex count, y: position offset, z: normal offset, w: stride (all in floats)
    float4 time;    // x: time
};

[[vk::binding(0, 0)]] RWStructuredBuffer<float> vertices : register(u0);
[[vk::binding(1, 0)]] ConstantBuffer<VertexUpdateParams> params : register(b1);

[shader("compute")]
[numthreads(64, 1, 1)]
void vertexUpdateMain(uint3 id : SV_DispatchThreadID)
{
    // vertex index - ignore if out of bounds (as they get batched into groups of 64)
    const uint index = id.x;
    if (index >= params.layout.x) {
        return;
    }

    // read the position and the normal from the vertex buffer
    const uint positionOffset = params.layout.y + index * params.layout.w;
    float3 position = float3(vertices[positionOffset], vertices[positionOffset + 1], vertices[positionOffset + 2]);
    const uint normalOffset = params.layout.z + index * params.layout.w;
    const float3 normal = float3(vertices[normalOffset], vertices[normalOffset + 1], vertices[normalOffset + 2]);

    // generate position from the normal by offsetting (0,0,0) by normal * strength
    const float time = params.time.x;
    const float3 strength = float3(
        1.0 + sin(time + 10.0 * position.y) * 0.1,
        1.0 + cos(time + 5.0 * position.x) * 0.1,
        1.0 + sin(time + 2.0 * position.z) * 0.2);
    position = normal * strength;

    // write the position back to the vertex buffer
    vertices[positionOffset + 0] = position.x;
    vertices[positionOffset + 1] = position.y;
    vertices[positionOffset + 2] = position.z;
}
)slang";

    struct VertexUpdateParams
    {
        uint32_t layout[4];
        float time[4];
    };
}

class VertexUpdateExample final : public ExampleApp
{
public:
    VertexUpdateExample() : ExampleApp({.title = "Compute Vertex Update", .miniStatsPresets = {"compute"}}) {}

protected:
    bool create() override
    {
        _colorAsset = std::make_unique<Asset>("color", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-color.jpg"));
        _normalAsset = std::make_unique<Asset>("normal", AssetType::TEXTURE,
            assetPath("textures/seaside-rocks01-normal.jpg"));
        _glossAsset = std::make_unique<Asset>("gloss", AssetType::TEXTURE, assetPath("textures/seaside-rocks01-gloss.jpg"));
        _envAsset = std::make_unique<Asset>("table-mountain-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* color = _colorAsset->resourceAs<Texture>();
        Texture* normal = _normalAsset->resourceAs<Texture>();
        Texture* gloss = _glossAsset->resourceAs<Texture>();
        Texture* env = _envAsset->resourceAs<Texture>();
        if (!color || !normal || !gloss || !env) {
            spdlog::error("vertex-update needs the seaside rocks textures and the table mountain atlas");
            return false;
        }

        // Setup skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(2.0f);
        scene()->setEnvAtlas(env);

        // Sphere material
        _material = std::make_shared<StandardMaterial>();
        _material->setDiffuseMap(color);
        _material->setNormalMap(normal);
        _material->setGlossMap(gloss);

        // Sphere mesh and entity
        _mesh = createMeshFromGeometry(device(), createSphereGeometry(1.0f, 100, 100));
        if (!_mesh) {
            spdlog::error("vertex-update: the sphere mesh could not be created");
            return false;
        }
        _sphere = new Entity();
        _sphere->setEngine(engine());
        _sphere->setName("Sphere");
        _sphere->addComponent<RenderComponent>();
        auto* render = _sphere->findComponent<RenderComponent>();
        _meshInstance = render->addMeshInstance(std::make_unique<MeshInstance>(_mesh, _material, _sphere));
        // The wobble reaches 1.2 from the centre
        _meshInstance->setCustomAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.25f, 1.25f, 1.25f)));
        root()->addChild(_sphere);

        // A camera with camera controls
        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 3.92f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        CameraControls* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f));
        controls->setEnableFly(false);
        controls->setPitchRange(Vector2(-90.0f, 90.0f));

        if (!device()->supportsCompute()) {
            spdlog::warn("vertex-update: this device has no compute; the sphere stays round");
            return true;
        }
        _shader = getOrCreateSlangShaderFromSource(device().get(), "vertex-update-compute", kVertexUpdateSlang);
        if (!_shader) {
            spdlog::error("vertex-update: the compute shader did not compile");
            return false;
        }

        // The position and normal offsets in the interleaved vertex, and its stride, in floats
        const auto vertexBuffer = _mesh->getVertexBuffer();
        const auto& format = *vertexBuffer->format();
        for (const auto& element : format.elements()) {
            if (element.semantic == VertexSemantic::SEMANTIC_POSITION) {
                _params.layout[1] = element.offset / 4u;
            } else if (element.semantic == VertexSemantic::SEMANTIC_NORMAL) {
                _params.layout[2] = element.offset / 4u;
            }
        }
        _params.layout[0] = static_cast<uint32_t>(vertexBuffer->numVertices());
        _params.layout[3] = static_cast<uint32_t>(format.size()) / 4u;

        // The compute shader, given the mesh's vertex buffer
        _compute = std::make_unique<Compute>(device().get(), _shader, "ComputeModifyVB");
        _compute->setParameter("vertices", vertexBuffer);
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        if (_compute) {
            _params.time[0] = _time;
            _compute->setUniformBlock(&_params, sizeof(_params));
            // A thread for each vertex
            _compute->setupDispatchThreads(_params.layout[0]);
            device()->computeDispatch({_compute.get()}, "ModifyVBDispatch");
        }

        // Solid / wireframe
        if (_meshInstance) {
            const bool wireframe = static_cast<int>(std::floor(_time * 0.5f)) % 2 != 0;
            _meshInstance->setRenderStyle(wireframe ? RENDERSTYLE_WIREFRAME : RENDERSTYLE_SOLID);
        }
    }

private:
    std::unique_ptr<Asset> _colorAsset;
    std::unique_ptr<Asset> _normalAsset;
    std::unique_ptr<Asset> _glossAsset;
    std::unique_ptr<Asset> _envAsset;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<Mesh> _mesh;
    std::shared_ptr<Shader> _shader;
    std::unique_ptr<Compute> _compute;
    Entity* _sphere = nullptr;
    MeshInstance* _meshInstance = nullptr;
    VertexUpdateParams _params{};
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(VertexUpdateExample)
