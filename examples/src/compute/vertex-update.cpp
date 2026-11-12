// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream compute/vertex-update.
//
// A compute shader rewrites the vertex buffer of a blob every frame. The mesh is a finely divided
// box (48 segments a side): the kernel moves each vertex onto the blob's surface along its
// direction from the centre - a unit sphere pushed in and out by a few waves travelling over it -
// which turns the box into a sphere with evenly spaced vertices and no crowded poles, and derives
// each normal from two small steps across the surface. The blob is polished metal with a thin
// iridescent film (0.6, film IOR 1.5, up to 550 nm) under the product studio's environment, the
// camera rendering no skybox against a dark clear colour; a soft key light, two coloured rim
// lights and a violet light from below outline it. The same mesh again, scaled up a little and
// drawn additively in glowing cyan lines without depth writes, is its wireframe. It turns slowly.
// The camera frame (4x MSAA, neutral tone mapping) adds bloom and a vignette; the HUD shows the
// frame's compute dispatches and passes.
//
// Keys stand in for upstream's controls panel:
//   [ / ]        the waves' amplitude -/+ 0.05 (0 - 0.75)
//   , / .        the speed -/+ 0.25 (0 - 3)
//   W            the wireframe on or off
//
// DEVIATIONS:
// - The vertex buffer is the engine's interleaved packed layout, so the kernel steps through it
//   by the vertex stride, where upstream asks for a separate (non-interleaved) storage layout.
// - The kernel is Slang: the buffer at index 0 and the uniform block after it (compute.h).
// - The subdivided box is built here (no BoxGeometry segments option); the iridescent film takes
//   upstream's 1.5 as its IOR.
//
#include <cmath>
#include <cstdint>
#include <memory>

#include "exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    // Moves every vertex onto the blob's surface along its direction, with the surface's normal.
    constexpr const char* kVertexUpdateSlang = R"slang(
struct VertexUpdateParams
{
    uint4 layout;     // x: vertex count, y: position offset, z: normal offset, w: stride (all in floats)
    float4 motion;    // x: time, y: amplitude
};

[[vk::binding(0, 0)]] RWStructuredBuffer<float> vertices : register(u0);
[[vk::binding(1, 0)]] ConstantBuffer<VertexUpdateParams> params : register(b1);

// The point of the blob's surface in a direction from its centre: a unit sphere, pushed in and out
// by a few waves travelling over it.
float3 surface(float3 dir)
{
    const float t = params.motion.x;
    const float swell = sin(dir.x * 3.0 + t * 1.1) * sin(dir.y * 2.5 + t * 1.3) * sin(dir.z * 3.0 + t * 0.9);
    const float ripple = sin(dir.y * 9.0 + dir.x * 3.0 - t * 2.0) * sin(dir.z * 7.0 + t * 1.5);
    return dir * (1.0 + params.motion.y * (swell + ripple * 0.15));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vertexUpdateMain(uint3 id : SV_DispatchThreadID)
{
    // vertex index - ignore if out of bounds (as they get batched into groups of 64)
    const uint index = id.x;
    if (index >= params.layout.x) {
        return;
    }

    // The surface only moves along the direction from the centre, so the direction of the
    // position is the same every frame.
    const uint positionOffset = params.layout.y + index * params.layout.w;
    const float3 dir = normalize(float3(vertices[positionOffset], vertices[positionOffset + 1],
        vertices[positionOffset + 2]));
    const float3 position = surface(dir);

    // The normal: the surface a small step away in two directions across dir; the cross product of
    // the two steps is perpendicular to it.
    const float3 up = abs(dir.y) > 0.9 ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
    const float3 tangent = normalize(cross(up, dir));
    const float3 bitangent = cross(dir, tangent);
    const float3 stepT = surface(normalize(dir + tangent * 0.01)) - position;
    const float3 stepB = surface(normalize(dir + bitangent * 0.01)) - position;
    const float3 normal = normalize(cross(stepT, stepB));

    const uint normalOffset = params.layout.z + index * params.layout.w;
    for (uint i = 0u; i < 3u; ++i) {
        vertices[positionOffset + i] = position[i];
        vertices[normalOffset + i] = normal[i];
    }
}
)slang";

    struct VertexUpdateParams
    {
        uint32_t layout[4];
        float motion[4];
    };

    // A unit box with `segments` divisions along each edge of each face; every face its own grid
    // of vertices, wound counter-clockwise seen from outside.
    PrimitiveGeometry subdividedBox(const int segments)
    {
        PrimitiveGeometry geometry;
        // Each face: its normal and two in-face axes, (u x v) = normal.
        const float faces[6][9] = {
            {1, 0, 0, 0, 0, -1, 0, 1, 0}, {-1, 0, 0, 0, 0, 1, 0, 1, 0},
            {0, 1, 0, 1, 0, 0, 0, 0, -1}, {0, -1, 0, 1, 0, 0, 0, 0, 1},
            {0, 0, 1, 1, 0, 0, 0, 1, 0},  {0, 0, -1, -1, 0, 0, 0, 1, 0},
        };
        for (const auto& f : faces) {
            const auto base = static_cast<uint32_t>(geometry.positions.size() / 3);
            for (int j = 0; j <= segments; ++j) {
                for (int i = 0; i <= segments; ++i) {
                    const float s = static_cast<float>(i) / segments - 0.5f;
                    const float t = static_cast<float>(j) / segments - 0.5f;
                    for (int c = 0; c < 3; ++c) {
                        geometry.positions.push_back(f[c] * 0.5f + f[3 + c] * s + f[6 + c] * t);
                        geometry.normals.push_back(f[c]);
                    }
                    geometry.uvs.push_back(static_cast<float>(i) / segments);
                    geometry.uvs.push_back(1.0f - static_cast<float>(j) / segments);
                }
            }
            const auto row = static_cast<uint32_t>(segments + 1);
            for (int j = 0; j < segments; ++j) {
                for (int i = 0; i < segments; ++i) {
                    const uint32_t a = base + static_cast<uint32_t>(j) * row + static_cast<uint32_t>(i);
                    geometry.indices.insert(geometry.indices.end(), {a, a + 1, a + row + 1, a, a + row + 1, a + row});
                }
            }
        }
        return geometry;
    }
}

class VertexUpdateExample final : public ExampleApp
{
public:
    VertexUpdateExample() : ExampleApp({.miniStatsPresets = {"compute"}}) {}

protected:
    bool create() override
    {
        _envAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/product-studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (Texture* env = _envAsset->resourceAs<Texture>()) {
            scene()->setEnvAtlas(env);
        }

        // The blob: a finely divided box, whose vertex buffer the kernel writes.
        _mesh = createMeshFromGeometry(device(), subdividedBox(48));
        if (!_mesh) {
            spdlog::error("vertex-update: the mesh could not be created");
            return false;
        }

        // Polished metal with a thin film, tinting the reflections with shifting rainbow colours.
        _material = std::make_shared<StandardMaterial>();
        _material->setDiffuse(Color(0.95f, 0.95f, 0.97f, 1.0f));
        _material->setUseMetalness(true);
        _material->setMetalness(1.0f);
        _material->setGloss(0.75f);
        _material->setIridescenceIntensity(0.6f);
        _material->setIridescenceIOR(1.5f);
        _material->setIridescenceThicknessMax(550.0f);

        _blob = new Entity();
        _blob->setName("Blob");
        _blob->setEngine(engine());
        _blob->addComponent<RenderComponent>();
        auto* blobInstance = _blob->findComponent<RenderComponent>()->addMeshInstance(
            std::make_unique<MeshInstance>(_mesh, _material, _blob));
        // The kernel moves the vertices past the box's bounds.
        blobInstance->setCustomAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.9f, 1.9f, 1.9f)));
        root()->addChild(_blob);

        // The wireframe: the same mesh, glowing lines added on top, scaled up a little so the
        // surface does not hide them.
        _wireMaterial = std::make_shared<StandardMaterial>();
        _wireMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _wireMaterial->setEmissive(Color().fromString("#4fd8ff"));
        _wireMaterial->setEmissiveIntensity(0.15f);
        _wireMaterial->setBlendState(std::make_shared<BlendState>(BlendState::additiveBlend()));
        auto depth = std::make_shared<DepthState>();
        depth->setDepthWrite(false);
        _wireMaterial->setDepthState(depth);
        _wireMaterial->setUseLighting(false);
        _wireMaterial->setUseSkybox(false);
        _wireframe = new Entity();
        _wireframe->setName("Wireframe");
        _wireframe->setEngine(engine());
        _wireframe->addComponent<RenderComponent>();
        auto* wireInstance = _wireframe->findComponent<RenderComponent>()->addMeshInstance(
            std::make_unique<MeshInstance>(_mesh, _wireMaterial, _wireframe));
        wireInstance->setCustomAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.9f, 1.9f, 1.9f)));
        wireInstance->setRenderStyle(RENDERSTYLE_WIREFRAME);
        _blob->addChild(_wireframe);
        _wireframe->setLocalScale(1.004f, 1.004f, 1.004f);

        // The camera renders no skybox: the background is the clear colour (linear, as the camera
        // frame reads it).
        Entity* camera = createCamera(Vector3(0.0f, 1.0f, 5.8f));
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
            controls->setZoomRange(Vector2(2.5f, 10.0f));
            controls->storeResetState();
        }
        auto rendering = cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 6;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.5f;
        cameraComponent->setRendering(rendering);

        // A soft key light from the upper left, two coloured rim lights from behind and a violet
        // light from below, seen from the starting camera.
        createLight("#fff1e0", 1.5f, Vector3(45.0f, -35.0f, 0.0f));
        createLight("#ff3da8", 3.0f, Vector3(-60.0f, 120.0f, 0.0f));
        createLight("#3d8bff", 3.0f, Vector3(-60.0f, -120.0f, 0.0f));
        createLight("#7a3dff", 2.5f, Vector3(150.0f, 0.0f, 0.0f));

        if (!device()->supportsCompute()) {
            spdlog::warn("vertex-update: this device has no compute; the blob stays a box");
            return true;
        }
        _shader = getOrCreateSlangShaderFromSource(device().get(), "vertex-update-compute", kVertexUpdateSlang);
        if (!_shader) {
            spdlog::error("vertex-update: the compute shader did not compile");
            return false;
        }

        // The position and normal offsets in the interleaved vertex, and its stride, in floats.
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

        _compute = std::make_unique<Compute>(device().get(), _shader, "ComputeModifyVB");
        _compute->setParameter("vertices", vertexBuffer);
        spdlog::info("vertex-update: [ ] amplitude, , . speed, W wireframe");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_LEFTBRACKET: _amplitude = std::max(0.0f, _amplitude - 0.05f); break;
        case SDLK_RIGHTBRACKET: _amplitude = std::min(0.75f, _amplitude + 0.05f); break;
        case SDLK_COMMA: _speed = std::max(0.0f, _speed - 0.25f); break;
        case SDLK_PERIOD: _speed = std::min(3.0f, _speed + 0.25f); break;
        case SDLK_W: _showWireframe = !_showWireframe; break;
        default: return false;
        }
        return true;
    }

    void update(const float dt) override
    {
        _time += dt * _speed;
        if (_compute) {
            _params.motion[0] = _time;
            _params.motion[1] = _amplitude;
            _compute->setUniformBlock(&_params, sizeof(_params));
            // A thread for each vertex.
            _compute->setupDispatchThreads(_params.layout[0]);
            device()->computeDispatch({_compute.get()}, "ModifyVBDispatch");
        }

        // The blob turns slowly; the wireframe shows or hides.
        _blob->setLocalEulerAngles(_time * 4.0f, _time * 10.0f, 0.0f);
        _wireframe->setEnabled(_showWireframe);
    }

private:
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

    std::unique_ptr<Asset> _envAsset;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<StandardMaterial> _wireMaterial;
    std::shared_ptr<Mesh> _mesh;
    std::shared_ptr<Shader> _shader;
    std::unique_ptr<Compute> _compute;
    Entity* _blob = nullptr;
    Entity* _wireframe = nullptr;
    VertexUpdateParams _params{};
    float _amplitude = 0.5f;
    float _speed = 1.0f;
    bool _showWireframe = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(VertexUpdateExample)
