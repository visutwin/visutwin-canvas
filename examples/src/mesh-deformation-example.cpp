// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/mesh-deformation.
//
// The statue, deformed on the CPU every frame: each vertex keeps its height and is pushed along
// x and z by 50 x sin(time + y x 0.01), in the mesh's own space, so the statue sways in a wave
// running up its height. Only the positions change; the normals stay as loaded. It is lit by the
// helipad environment atlas alone (skybox mip 2, exposure 1), and the camera circles it at a
// radius of 25 and a height of 15, looking at (0, 7, 0).
//
// Each mesh keeps its loaded positions, and a mesh several instances share is deformed once. A
// deformed mesh is drawn from vertex buffers of its own, one per frame in flight, so a frame the
// GPU is still drawing never has its vertices rewritten under it, and its bounds follow the
// deformed positions.
//
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include "../exampleApp.h"
#include "core/shape/boundingBox.h"
#include "framework/assets/asset.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    struct DeformedMesh
    {
        Mesh* mesh = nullptr;
        // The loaded positions, xyz per vertex
        std::vector<float> srcPositions;
        // The loaded vertices, rewritten with the deformed positions every frame
        std::vector<uint8_t> vertices;
        size_t stride = 0;
        size_t positionOffset = 0;
        std::vector<std::shared_ptr<VertexBuffer>> buffers;
    };
}

class MeshDeformationExample final: public ExampleApp
{
public:
    MeshDeformationExample(): ExampleApp({.title = "Mesh Deformation"}) {}

protected:
    bool create() override
    {
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _statue = std::make_unique<Asset>("statue", AssetType::CONTAINER, assetPath("models/statue.glb"));

        Texture* helipad = _helipad->resourceAs<Texture>();
        ContainerResource* statue = _statue->resourceAs<ContainerResource>();
        if (!helipad || !statue) {
            spdlog::error("Failed to load the helipad environment atlas or the statue");
            return false;
        }

        // The skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(1.0f);
        scene()->setEnvAtlas(helipad);

        // The camera
        _camera = createCamera(Vector3(0.0f, 7.0f, 24.0f));
        if (auto* component = _camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        }

        // The hierarchy of entities with render components that is the statue
        Entity* entity = statue->instantiateRenderEntity();
        if (!entity) {
            spdlog::error("The statue has no render entity");
            return false;
        }
        root()->addChild(entity);

        // Collect the positions of every mesh to work on, each mesh once
        const int bufferCount = std::max(1, device()->maxFramesInFlight());
        std::unordered_set<Mesh*> seen;
        for (auto* render : entity->findComponents<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                Mesh* mesh = meshInstance ? meshInstance->mesh() : nullptr;
                if (!mesh || !seen.insert(mesh).second) {
                    continue;
                }
                if (auto deformed = collect(mesh, bufferCount)) {
                    _meshes.push_back(std::move(*deformed));
                }
            }
        }
        spdlog::info("Deforming {} meshes", _meshes.size());

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Orbit the camera
        _camera->setLocalPosition(25.0f * std::sin(_time * 0.2f), 15.0f, 25.0f * std::cos(_time * 0.2f));
        _camera->lookAt(Vector3(0.0f, 7.0f, 0.0f));

        constexpr float strength = 50.0f;
        ++_bufferIndex;

        // Modify the mesh positions every frame: x and z waved by a sine of y
        for (DeformedMesh& deformed : _meshes) {
            const size_t count = deformed.srcPositions.size() / 3;
            Vector3 min;
            Vector3 max;
            for (size_t k = 0; k < count; ++k) {
                const float* src = &deformed.srcPositions[k * 3];
                const float wave = strength * std::sin(_time + src[1] * 0.01f);
                const float position[3] = {src[0] + wave, src[1], src[2] + wave};
                std::memcpy(deformed.vertices.data() + k * deformed.stride + deformed.positionOffset, position,
                    sizeof(position));

                const Vector3 p(position[0], position[1], position[2]);
                min = k == 0 ? p : Vector3::min(min, p);
                max = k == 0 ? p : Vector3::max(max, p);
            }

            const auto& buffer = deformed.buffers[_bufferIndex % deformed.buffers.size()];
            buffer->writeRange(0, deformed.vertices.data(), deformed.vertices.size());
            deformed.mesh->setVertexBuffer(buffer);
            deformed.mesh->setAabb(BoundingBox((min + max) * 0.5f, (max - min) * 0.5f));
        }
    }

private:
    // A mesh's loaded vertices and positions, and the buffers it is deformed into. Nothing for a
    // mesh with no CPU copy of its vertices or with positions that are not three floats.
    std::optional<DeformedMesh> collect(Mesh* mesh, const int bufferCount) const
    {
        const auto source = mesh->getVertexBuffer();
        if (!source || !source->format() || source->storage().empty()) {
            return std::nullopt;
        }
        const auto& format = source->format();
        const VertexElement* position = nullptr;
        for (const VertexElement& element : format->elements()) {
            if (element.semantic == VertexSemantic::SEMANTIC_POSITION) {
                position = &element;
                break;
            }
        }
        if (!position || position->dataType != VertexDataType::TYPE_FLOAT32 || position->componentCount < 3) {
            spdlog::warn("A statue mesh's positions are not three floats; it is left undeformed");
            return std::nullopt;
        }

        DeformedMesh deformed;
        deformed.mesh = mesh;
        deformed.stride = static_cast<size_t>(format->size());
        deformed.positionOffset = position->offset;
        const size_t count = std::min(static_cast<size_t>(source->numVertices()),
            source->storage().size() / deformed.stride);
        deformed.vertices.assign(source->storage().begin(),
            source->storage().begin() + static_cast<std::ptrdiff_t>(count * deformed.stride));
        deformed.srcPositions.resize(count * 3);
        for (size_t k = 0; k < count; ++k) {
            std::memcpy(&deformed.srcPositions[k * 3],
                deformed.vertices.data() + k * deformed.stride + deformed.positionOffset, 3 * sizeof(float));
        }

        for (int i = 0; i < bufferCount; ++i) {
            VertexBufferOptions options;
            options.usage = BUFFER_DYNAMIC;
            options.data = deformed.vertices;
            deformed.buffers.push_back(device()->createVertexBuffer(format, static_cast<int>(count), options));
        }
        return deformed;
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _statue;
    std::vector<DeformedMesh> _meshes;
    Entity* _camera = nullptr;
    size_t _bufferIndex = 0;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MeshDeformationExample)
