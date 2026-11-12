// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/mesh-deformation.
//
// The statue, deformed on the CPU every frame, in a dark studio. Each vertex keeps its height and
// is pushed round a circle in x and z (sin and cos of one phase), in the mesh's own space: with t
// the vertex's height between the lowest and highest vertex of all the meshes, the phase is
// 1.5 x time + 4 t and the radius 0.09 x that height range x t^2, so the base stays anchored and a
// wave runs up the statue, swaying it most at the top. Only the positions change; the normals stay
// as loaded.
//
// The helipad environment atlas (skybox mip 2, intensity 0.35, exposure 1) gives only soft ambient
// light: the skybox layer is disabled and the camera clears to a dark blue grey. A warm key light
// (50, 35, intensity 2.2) casts soft VSM shadows of the statue onto itself (2048, blur 15, distance
// four statue heights, bias 0.2, normal offset 0.1), and a cool rim light (20, 215, intensity 1.6)
// lights it from behind. The camera (field of view 40) renders through a camera frame with ACES
// tone mapping, bloom 0.02 and a vignette of intensity 0.6, and circles the statue at 1.9 statue
// heights, at 0.7 of its height, looking just below the centre of its bounds.
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
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include "exampleApp.h"
#include "core/shape/boundingBox.h"
#include "framework/assets/asset.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
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
    MeshDeformationExample(): ExampleApp({}) {}

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

        // Dark studio look: the environment only provides soft ambient light, the skybox itself
        // is hidden
        const Color backgroundColor(0.035f, 0.04f, 0.06f, 1.0f);
        scene()->setSkyboxMip(2);
        scene()->setExposure(1.0f);
        scene()->setSkyboxIntensity(0.35f);
        scene()->setEnvAtlas(helipad);
        if (const auto layers = scene()->layers()) {
            if (const auto skyLayer = layers->getLayerById(LAYERID_SKYBOX)) {
                skyLayer->setEnabled(false);
            }
        }

        // The hierarchy of entities with render components that is the statue, casting and
        // receiving shadows
        Entity* entity = statue->instantiateRenderEntity();
        if (!entity) {
            spdlog::error("The statue has no render entity");
            return false;
        }
        root()->addChild(entity);
        for (auto* render : entity->findComponents<RenderComponent>()) {
            render->setCastShadows(true);
            render->setReceiveShadows(true);
        }

        // World space bounds of the statue, used to frame the camera
        _bounds = entityBounds(entity);
        _height = _bounds.halfExtents().getY() * 2.0f;

        // The camera, where the orbit starts, with tone mapping, bloom and a vignette
        _camera = createCamera(Vector3(0.0f, _height * 0.7f, _height * 1.9f));
        if (auto* component = _camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(backgroundColor);
            component->camera()->setFov(40.0f);
            component->setToneMapping(TONEMAP_ACES);
            auto rendering = component->rendering();
            rendering.bloomIntensity = 0.02f;
            rendering.vignetteEnabled = true;
            rendering.vignetteIntensity = 0.6f;
            component->setRendering(rendering);
        }

        // Key light with soft self-shadowing
        Entity* key = createDirectionalLight(Vector3(50.0f, 35.0f, 0.0f), Color(1.0f, 0.93f, 0.82f, 1.0f), 2.2f, true);
        key->setName("key");
        if (auto* light = key->findComponent<LightComponent>()) {
            light->setShadowResolution(2048);
            light->setShadowType(SHADOW_VSM_16F);
            light->setVsmBlurSize(15);
            light->setShadowDistance(_height * 4.0f);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.1f);
        }

        // Cool rim light from behind
        Entity* rim = createDirectionalLight(Vector3(20.0f, 215.0f, 0.0f), Color(0.35f, 0.55f, 1.0f, 1.0f), 1.6f);
        rim->setName("rim");

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

        // The statue is anchored at its base: the deformation grows towards the top
        float minY = std::numeric_limits<float>::infinity();
        float maxY = -std::numeric_limits<float>::infinity();
        for (const DeformedMesh& deformed : _meshes) {
            for (size_t k = 1; k < deformed.srcPositions.size(); k += 3) {
                minY = std::min(minY, deformed.srcPositions[k]);
                maxY = std::max(maxY, deformed.srcPositions[k]);
            }
        }
        _minY = minY;
        _range = maxY - minY;
        _strength = _range * 0.09f;

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Orbit the camera
        const float r = _height * 1.9f;
        _camera->setLocalPosition(r * std::sin(_time * 0.15f), _height * 0.7f, r * std::cos(_time * 0.15f));
        const Vector3& center = _bounds.center();
        _camera->lookAt(Vector3(center.getX(), center.getY() - _height * 0.05f, center.getZ()));

        if (!(_range > 0.0f)) {
            return;
        }
        ++_bufferIndex;

        // Modify the mesh positions every frame
        for (DeformedMesh& deformed : _meshes) {
            const size_t count = deformed.srcPositions.size() / 3;
            Vector3 min;
            Vector3 max;
            for (size_t k = 0; k < count; ++k) {
                // A wave travels up the statue, displacing x and z (a circular sway), with the
                // amount growing towards the top
                const float* src = &deformed.srcPositions[k * 3];
                const float y = src[1];
                const float t = (y - _minY) / _range;
                const float phase = _time * 1.5f + t * 4.0f;
                const float amount = _strength * t * t;
                const float position[3] = {src[0] + amount * std::sin(phase), y, src[2] + amount * std::cos(phase)};
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
    BoundingBox _bounds;
    float _height = 1.0f;
    float _minY = 0.0f;
    float _range = 0.0f;
    float _strength = 0.0f;
    size_t _bufferIndex = 0;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MeshDeformationExample)
