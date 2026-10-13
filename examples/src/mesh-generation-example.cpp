// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/mesh-generation.
//
// A 60x60 grid mesh 20 units across, built on the CPU and rebuilt every frame: four coloured
// omni lights (range 10, no shadows) circle above it at different radii and speeds, each
// marked by a sphere glowing in the light's colour, and the grid rises under each light in a
// smoothstep bump whose radius is that light's own. The smooth normals are recomputed from the
// positions every frame. The grid wears the grey logo texture with gloss 0.5 and metalness 0.3
// in the metalness workflow; an ambient of 0.1, and the camera at (0, 5, 20) looks at the origin.
//
// The vertex buffer is rewritten every frame, so the mesh cycles through one buffer per frame in
// flight: a frame the GPU is still drawing never has its vertices rewritten under it.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "core/shape/boundingBox.h"
#include "framework/assets/asset.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/geometry/geometryUtils.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // The grid: world size 20, 60 vertices along each side
    constexpr int kResolution = 60;
    constexpr float kExtent = 20.0f;
    constexpr float kScale = kExtent / kResolution;
    constexpr int kVertexCount = kResolution * kResolution;

    // The engine's packed vertex: position, normal, uv0, tangent (xyzw), uv1
    constexpr int kFloatsPerVertex = 14;

    float smoothstep(const float min, const float max, float x)
    {
        if (x <= min) {
            return 0.0f;
        }
        if (x >= max) {
            return 1.0f;
        }
        x = (x - min) / (max - min);
        return x * x * (3.0f - 2.0f * x);
    }

    struct MovingLight
    {
        float radius;
        float speed;
        float scale;
        Entity* entity = nullptr;
    };
}

class MeshGenerationExample final: public ExampleApp
{
public:
    MeshGenerationExample(): ExampleApp({.title = "Mesh Generation"}) {}

protected:
    bool create() override
    {
        _greyAsset = std::make_unique<Asset>("playcanvasGrey", AssetType::TEXTURE,
            assetPath("textures/playcanvas-grey.png"));
        Texture* grey = _greyAsset->resourceAs<Texture>();
        if (!grey) {
            spdlog::error("Failed to load textures/playcanvas-grey.png");
            return false;
        }

        scene()->setAmbientLight(0.1f, 0.1f, 0.1f);

        // Four lights that move over the scene and deform the mesh as well
        _lights = {{
            {7.0f, 1.0f, 2.5f, createLight(Color(0.3f, 0.9f, 0.6f, 1.0f), 1.0f)},
            {3.0f, 1.2f, 3.0f, createLight(Color(0.7f, 0.2f, 0.3f, 1.0f), 1.3f)},
            {5.0f, -0.8f, 4.0f, createLight(Color(0.2f, 0.2f, 0.9f, 1.0f), 1.5f)},
            {4.0f, -0.3f, 5.5f, createLight(Color(0.8f, 0.9f, 0.4f, 1.0f), 1.7f)},
        }};

        // The camera
        auto* camera = createCamera(Vector3(0.0f, 5.0f, 20.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
        }
        camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // Positions and uv coordinates of a flat grid
        _positions.resize(3 * kVertexCount);
        _uvs.resize(2 * kVertexCount);
        int index = 0;
        for (int x = 0; x < kResolution; ++x) {
            for (int z = 0; z < kResolution; ++z) {
                _positions[3 * index] = kScale * (static_cast<float>(x) - kResolution * 0.5f);
                _positions[3 * index + 1] = 0.0f;
                _positions[3 * index + 2] = kScale * (static_cast<float>(z) - kResolution * 0.5f);
                _uvs[2 * index] = static_cast<float>(x) / kResolution;
                _uvs[2 * index + 1] = 1.0f - static_cast<float>(z) / kResolution;
                ++index;
            }
        }

        // A triangle list, two triangles per grid square
        for (int x = 0; x < kResolution - 1; ++x) {
            for (int y = 0; y < kResolution - 1; ++y) {
                const auto at = [](const int i, const int j) { return static_cast<uint32_t>(i * kResolution + j); };
                _indices.insert(_indices.end(), {
                    at(x, y + 1), at(x + 1, y), at(x, y),
                    at(x + 1, y), at(x, y + 1), at(x + 1, y + 1)});
            }
        }

        // The vertex buffers, one per frame in flight, and the static index buffer
        _vertices.assign(static_cast<size_t>(kVertexCount) * kFloatsPerVertex, 0.0f);
        auto format = std::make_shared<VertexFormat>(kFloatsPerVertex * static_cast<int>(sizeof(float)),
            VertexFormat::standardElements());
        const int bufferCount = std::max(1, device()->maxFramesInFlight());
        for (int i = 0; i < bufferCount; ++i) {
            VertexBufferOptions options;
            options.usage = BUFFER_DYNAMIC;
            options.data.resize(_vertices.size() * sizeof(float));
            _vertexBuffers.push_back(device()->createVertexBuffer(format, kVertexCount, options));
        }

        std::vector<uint8_t> indexBytes(_indices.size() * sizeof(uint32_t));
        std::memcpy(indexBytes.data(), _indices.data(), indexBytes.size());
        auto indexBuffer = device()->createIndexBuffer(INDEXFORMAT_UINT32, static_cast<int>(_indices.size()),
            indexBytes);

        _mesh = std::make_shared<Mesh>();
        _mesh->setIndexBuffer(indexBuffer);
        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.indexed = true;
        primitive.base = 0;
        primitive.count = static_cast<int>(_indices.size());
        _mesh->setPrimitive(primitive);
        updateMesh();

        // The material
        _material = std::make_shared<StandardMaterial>();
        _material->setDiffuseMap(grey);
        _material->setGloss(0.5f);
        _material->setMetalness(0.3f);
        _material->setUseMetalness(true);

        // The entity drawing the mesh
        auto* entity = new Entity();
        entity->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
            render->setMaterial(_material.get());
            render->addMeshInstance(std::make_unique<MeshInstance>(_mesh.get(), _material.get(), entity));
        }
        root()->addChild(entity);

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Move the lights along circles, keeping their positions for the deformation below
        std::array<Vector2, 4> lightPositions;
        for (size_t l = 0; l < _lights.size(); ++l) {
            const MovingLight& light = _lights[l];
            lightPositions[l] = Vector2(light.radius * std::sin(_time * light.speed),
                light.radius * std::cos(_time * light.speed));
            light.entity->setLocalPosition(lightPositions[l].x, 3.0f, lightPositions[l].y);
        }

        // Raise each grid vertex where a light is close
        for (int index = 0; index < 3 * kVertexCount; index += 3) {
            float elevation = 0.0f;
            for (size_t l = 0; l < _lights.size(); ++l) {
                const float dx = _positions[index] - lightPositions[l].x;
                const float dz = _positions[index + 2] - lightPositions[l].y;
                float distance = std::sqrt(dx * dx + dz * dz);
                distance = std::clamp(distance, 0.0f, _lights[l].scale);
                distance = smoothstep(0.0f, _lights[l].scale, distance);
                elevation += 1.0f - distance;
            }
            _positions[index + 1] = elevation;
        }

        updateMesh();
    }

private:
    // An omni light, and a sphere glowing in its colour at its position
    Entity* createLight(const Color& color, const float scale)
    {
        auto* light = new Entity();
        light->setEngine(engine());
        if (auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            component->setType(LightType::LIGHTTYPE_OMNI);
            component->setColor(color);
            component->setRange(10.0f);
            component->setCastShadows(false);
        }

        auto material = std::make_shared<StandardMaterial>();
        material->setEmissive(color);
        _lightMaterials.push_back(material);

        if (auto* render = static_cast<RenderComponent*>(light->addComponent<RenderComponent>())) {
            render->setMaterial(material.get());
            render->setType("sphere");
        }
        light->setLocalScale(scale, scale, scale);

        root()->addChild(light);
        return light;
    }

    // Writes the positions, freshly computed normals and the uvs into the next frame's vertex
    // buffer, points the mesh at it, and bounds the mesh by the positions
    void updateMesh()
    {
        const std::vector<float> normals = calculateNormals(_positions, _indices);

        Vector3 min(_positions[0], _positions[1], _positions[2]);
        Vector3 max = min;
        for (int i = 0; i < kVertexCount; ++i) {
            float* v = &_vertices[static_cast<size_t>(i) * kFloatsPerVertex];
            const Vector3 position(_positions[3 * i], _positions[3 * i + 1], _positions[3 * i + 2]);
            min = Vector3::min(min, position);
            max = Vector3::max(max, position);
            v[0] = _positions[3 * i];
            v[1] = _positions[3 * i + 1];
            v[2] = _positions[3 * i + 2];
            v[3] = normals[3 * i];
            v[4] = normals[3 * i + 1];
            v[5] = normals[3 * i + 2];
            v[6] = _uvs[2 * i];
            v[7] = _uvs[2 * i + 1];
            // No normal map reads the tangent; a valid frame along +X
            v[8] = 1.0f;
            v[9] = 0.0f;
            v[10] = 0.0f;
            v[11] = 1.0f;
            v[12] = _uvs[2 * i];
            v[13] = _uvs[2 * i + 1];
        }

        _bufferIndex = (_bufferIndex + 1) % _vertexBuffers.size();
        const auto& buffer = _vertexBuffers[_bufferIndex];
        buffer->writeRange(0, _vertices.data(), _vertices.size() * sizeof(float));
        _mesh->setVertexBuffer(buffer);
        _mesh->setAabb(BoundingBox((min + max) * 0.5f, (max - min) * 0.5f));
    }

    std::unique_ptr<Asset> _greyAsset;
    std::shared_ptr<StandardMaterial> _material;
    std::vector<std::shared_ptr<StandardMaterial>> _lightMaterials;
    std::array<MovingLight, 4> _lights{};
    std::shared_ptr<Mesh> _mesh;
    std::vector<std::shared_ptr<VertexBuffer>> _vertexBuffers;
    size_t _bufferIndex = 0;
    std::vector<float> _positions;
    std::vector<float> _uvs;
    std::vector<uint32_t> _indices;
    std::vector<float> _vertices;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MeshGenerationExample)
