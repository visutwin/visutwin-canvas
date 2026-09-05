// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream graphics/mesh-morph.
//
// Three high-band spheres, each carrying three morph targets built by extruding the
// part of the sphere nearest a plane along its own normals: one target per axis
// plane. The weights are driven by three sine curves of different frequency, so the
// spheres bulge along each axis in turn and the blend between targets is what is on
// show.
//
// This is the only example that exercises morph weights, and the targets are built
// here rather than loaded, so it also covers the CPU side of MorphTarget.
//
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/morph.h"
#include "scene/morphInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float PI_F = 3.14159265358979323846f;
    constexpr int kBands = 64;

    /// The sphere's own vertex data, kept on the CPU because the morph targets are
    /// deltas against it.
    struct SphereData
    {
        std::vector<float> positions;   // packed xyz
        std::vector<float> normals;     // packed xyz
        std::vector<uint32_t> indices;
    };

    SphereData buildSphere(const int bands)
    {
        SphereData data;
        constexpr float radius = 0.5f;
        for (int lat = 0; lat <= bands; ++lat) {
            const float theta = static_cast<float>(lat) * PI_F / static_cast<float>(bands);
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);
            for (int lon = 0; lon <= bands; ++lon) {
                const float phi = static_cast<float>(lon) * 2.0f * PI_F / static_cast<float>(bands);
                const float nx = std::cos(phi) * sinTheta;
                const float ny = cosTheta;
                const float nz = std::sin(phi) * sinTheta;
                data.positions.insert(data.positions.end(), {radius * nx, radius * ny, radius * nz});
                data.normals.insert(data.normals.end(), {nx, ny, nz});
            }
        }
        for (int lat = 0; lat < bands; ++lat) {
            for (int lon = 0; lon < bands; ++lon) {
                const auto first = static_cast<uint32_t>(lat * (bands + 1) + lon);
                const auto second = static_cast<uint32_t>(first + bands + 1);
                // Counter-clockwise seen from OUTSIDE, which is what makes the
                // generated normals agree with the engine's front face. The
                // opposite order renders a sphere whose normals point inward: it
                // is not obviously wrong on a mirror ball, but a diffuse one comes
                // out black.
                data.indices.insert(data.indices.end(), {first, first + 1u, second});
                data.indices.insert(data.indices.end(), {second, first + 1u, second + 1u});
            }
        }
        return data;
    }

    float smoothstep(const float edge0, const float edge1, const float x)
    {
        const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    /// Distance from a point to the plane through the origin with this normal.
    float shortestDistance(const float x, const float y, const float z,
        const float nx, const float ny, const float nz)
    {
        const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        return length > 0.0f ? std::abs(x * nx + y * ny + z * nz) / length : 0.0f;
    }

    /// Face normals accumulated onto vertices, so a displaced target still lights
    /// correctly rather than keeping the sphere's own normals.
    std::vector<float> calculateNormals(const std::vector<float>& positions,
        const std::vector<uint32_t>& indices)
    {
        std::vector<float> normals(positions.size(), 0.0f);
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const uint32_t a = indices[i], b = indices[i + 1], c = indices[i + 2];
            const float e1[3] = {positions[b * 3] - positions[a * 3],
                positions[b * 3 + 1] - positions[a * 3 + 1],
                positions[b * 3 + 2] - positions[a * 3 + 2]};
            const float e2[3] = {positions[c * 3] - positions[a * 3],
                positions[c * 3 + 1] - positions[a * 3 + 1],
                positions[c * 3 + 2] - positions[a * 3 + 2]};
            const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            for (const uint32_t v : {a, b, c}) {
                normals[v * 3] += n[0];
                normals[v * 3 + 1] += n[1];
                normals[v * 3 + 2] += n[2];
            }
        }
        for (size_t i = 0; i + 2 < normals.size(); i += 3) {
            const float len = std::sqrt(normals[i] * normals[i] +
                normals[i + 1] * normals[i + 1] + normals[i + 2] * normals[i + 2]);
            if (len > 1e-8f) {
                normals[i] /= len;
                normals[i + 1] /= len;
                normals[i + 2] /= len;
            }
        }
        return normals;
    }

    /// One target: push the vertices NEAR the plane out along their own normals, so
    /// the sphere grows a bulge facing that axis. Targets store deltas, not
    /// absolute positions.
    MorphTarget createMorphTarget(const SphereData& base,
        const float nx, const float ny, const float nz)
    {
        constexpr float limit = 0.2f;
        std::vector<float> modified(base.positions.size());
        for (size_t i = 0; i + 2 < base.positions.size(); i += 3) {
            const float dist = shortestDistance(base.positions[i], base.positions[i + 1],
                base.positions[i + 2], nx, ny, nz);
            const float displacement = 1.0f - smoothstep(0.0f, limit, dist);
            modified[i] = base.positions[i] + base.normals[i] * displacement;
            modified[i + 1] = base.positions[i + 1] + base.normals[i + 1] * displacement;
            modified[i + 2] = base.positions[i + 2] + base.normals[i + 2] * displacement;
        }

        std::vector<float> modifiedNormals = calculateNormals(modified, base.indices);
        for (size_t i = 0; i < modified.size(); ++i) {
            modified[i] -= base.positions[i];
            modifiedNormals[i] -= base.normals[i];
        }

        MorphTarget target;
        target.deltaPositions = std::move(modified);
        target.deltaNormals = std::move(modifiedNormals);
        return target;
    }
}

class MeshMorphExample final: public ExampleApp
{
public:
    MeshMorphExample(): ExampleApp({.title = "Mesh Morph"}) {}

protected:
    bool create() override
    {
        scene()->setToneMapping(TONEMAP_ACES);
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.5f, false);

        const SphereData base = buildSphere(kBands);

        std::uniform_real_distribution<float> spread(-3.0f, 3.0f);
        for (int k = 0; k < 3; ++k) {
            createMorphedSphere(base, Vector3(spread(_rng), spread(_rng), spread(_rng)));
        }

        // Upstream orbits the camera itself at radius 16; the orbit controls do the
        // same job here and let the scene be inspected.
        auto* camera = createCamera(Vector3(0.0f, 4.0f, 16.0f));
        auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f));
        controls->setOrbitDistance(16.5f);
        controls->storeResetState();

        spdlog::info("Three spheres, three morph targets each, weights driven by "
                     "sine curves of different frequency.");
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        for (size_t m = 0; m < _morphInstances.size(); ++m) {
            auto& instance = _morphInstances[m];
            const float offset = static_cast<float>(m);
            instance->setWeight(0, std::abs(std::sin(_time + offset)));
            instance->setWeight(1, std::abs(std::sin(_time * 0.3f + offset)));
            instance->setWeight(2, std::abs(std::sin(_time * 0.7f + offset)));
        }
    }

private:
    void createMorphedSphere(const SphereData& base, const Vector3& position)
    {
        const int vertexCount = static_cast<int>(base.positions.size() / 3);

        // Interleaved standard vertex: position(3) normal(3) uv0(2) tangent(4) uv1(2).
        std::vector<float> interleaved;
        interleaved.reserve(static_cast<size_t>(vertexCount) * 14);
        for (int i = 0; i < vertexCount; ++i) {
            const float nx = base.normals[i * 3];
            const float ny = base.normals[i * 3 + 1];
            const float nz = base.normals[i * 3 + 2];
            interleaved.insert(interleaved.end(), {
                base.positions[i * 3], base.positions[i * 3 + 1], base.positions[i * 3 + 2],
                nx, ny, nz,
                0.0f, 0.0f,
                -nz, 0.0f, nx, 1.0f,
                0.0f, 0.0f});
        }

        auto format = std::make_shared<VertexFormat>(
            56, VertexFormat::standardElements(), true, false);
        VertexBufferOptions vbOptions;
        vbOptions.data.resize(interleaved.size() * sizeof(float));
        std::memcpy(vbOptions.data.data(), interleaved.data(), vbOptions.data.size());
        auto vertexBuffer = device()->createVertexBuffer(format, vertexCount, vbOptions);

        std::vector<uint8_t> indexBytes(base.indices.size() * sizeof(uint32_t));
        std::memcpy(indexBytes.data(), base.indices.data(), indexBytes.size());
        auto indexBuffer = device()->createIndexBuffer(
            INDEXFORMAT_UINT32, static_cast<int>(base.indices.size()), indexBytes);

        auto mesh = std::make_shared<Mesh>();
        mesh->setVertexBuffer(vertexBuffer);
        mesh->setIndexBuffer(indexBuffer);
        Primitive prim;
        prim.type = PRIMITIVE_TRIANGLES;
        prim.indexed = true;
        prim.base = 0;
        prim.count = static_cast<int>(base.indices.size());
        mesh->setPrimitive(prim);
        // The bulges reach past the base sphere, so the bounds have to allow for
        // them or a morphed sphere is culled at the wrong moment.
        mesh->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.6f, 1.6f, 1.6f)));
        _meshes.push_back(mesh);

        std::vector<MorphTarget> targets;
        targets.push_back(createMorphTarget(base, 1.0f, 0.0f, 0.0f));
        targets.push_back(createMorphTarget(base, 0.0f, 1.0f, 0.0f));
        targets.push_back(createMorphTarget(base, 0.0f, 0.0f, 1.0f));
        auto morph = std::make_shared<Morph>(std::move(targets), vertexCount, device().get());
        _morphs.push_back(morph);

        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(0.6f, 0.65f, 0.75f, 1.0f));
        material->setMetalness(0.0f);
        material->setGloss(0.5f);
        _materials.push_back(material);

        auto* entity = new Entity();
        entity->setEngine(engine());
        entity->setLocalPosition(position);
        root()->addChild(entity);

        if (auto* render = static_cast<RenderComponent*>(
                entity->addComponent<RenderComponent>())) {
            auto instance = std::make_unique<MeshInstance>(mesh, material, entity);
            auto morphInstance = std::make_shared<MorphInstance>(morph);
            instance->setMorphInstance(morphInstance);
            _morphInstances.push_back(morphInstance);
            render->addMeshInstance(std::move(instance));
        }
    }

    std::vector<std::shared_ptr<Mesh>> _meshes;
    std::vector<std::shared_ptr<Morph>> _morphs;
    std::vector<std::shared_ptr<MorphInstance>> _morphInstances;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::mt19937 _rng{20260905};
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MeshMorphExample)
