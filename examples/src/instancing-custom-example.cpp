// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/instancing-custom.
//
// 3000 small spheres drawn in one instanced draw, each at a random position in a 10-unit cube
// with a random scale between 0.1 and 0.2, pulled toward a large sphere (scale 2) that bobs up
// and down through the middle of the cloud: an instance at distance d from the large sphere's
// centre moves toward it by exp(-0.2 d) of the way, so the spheres nearest it crowd onto it. The
// instanced spheres and the large one share one metallic material (gloss 0.5, metalness 1,
// diffuse (0.7, 0.5, 0.7)), lit by the table mountain environment atlas (skybox mip 2, exposure
// 0.8) and an ambient of 0.1, tone mapped with ACES. The camera circles the cloud.
//
// DEVIATION: upstream keeps each instance's position and scale in a custom per-instance vertex
// format and moves the instances in a custom instancing vertex shader chunk, fed the centre and
// time as material parameters. A material here has neither custom per-instance attributes nor
// named parameters, and instancing takes only the matrix format, so this port evaluates the same
// formula on the CPU every frame, writes the 3000 matrices into the next of one dynamic instance
// buffer per frame in flight, and hands it to the mesh instance again, which also keeps the
// instancing bounds following the instances.
// DEVIATION: upstream places the instances with Math.random; this port uses a fixed-seed
// std::mt19937, so the cloud is the same in every run.
//
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kInstanceCount = 3000;

    // One instance: its position and its scale
    struct InstanceData
    {
        Vector3 position;
        float scale = 1.0f;
    };
}

class InstancingCustomExample final: public ExampleApp
{
public:
    InstancingCustomExample(): ExampleApp({.title = "Instancing Custom"}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* envAtlas = _envAtlas->resourceAs<Texture>();
        if (!envAtlas) {
            spdlog::error("Failed to load cubemaps/table-mountain-env-atlas.png");
            return false;
        }

        // The skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(0.8f);
        scene()->setEnvAtlas(envAtlas);

        // General scene rendering properties
        scene()->setAmbientLight(0.1f, 0.1f, 0.1f);

        // The camera
        _camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        if (auto* component = _camera->findComponent<CameraComponent>()) {
            component->setToneMapping(TONEMAP_ACES);
        }

        // The data of each instance
        constexpr float range = 10.0f;
        std::mt19937 rng(2026u);
        std::uniform_real_distribution<float> random(0.0f, 1.0f);
        _instances.resize(kInstanceCount);
        for (InstanceData& instance : _instances) {
            const float x = random(rng) * range - range * 0.5f;
            const float y = random(rng) * range - range * 0.5f;
            const float z = random(rng) * range - range * 0.5f;
            instance.position = Vector3(x, y, z);
            instance.scale = 0.1f + random(rng) * 0.1f;
        }

        // The instance matrix buffers, one per frame in flight
        _matrices.resize(static_cast<size_t>(kInstanceCount) * 16);
        const int bufferCount = std::max(1, device()->maxFramesInFlight());
        for (int i = 0; i < bufferCount; ++i) {
            VertexBufferOptions options;
            options.usage = BUFFER_DYNAMIC;
            options.data.resize(_matrices.size() * sizeof(float));
            _instanceBuffers.push_back(device()->createVertexBuffer(VertexFormat::defaultInstancingFormat(),
                kInstanceCount, options));
        }

        // A standard material, used for the instanced and the non-instanced sphere alike
        _material = std::make_shared<StandardMaterial>();
        _material->setGloss(0.5f);
        _material->setMetalness(1.0f);
        _material->setDiffuse(Color(0.7f, 0.5f, 0.7f, 1.0f));
        _material->setUseMetalness(true);

        // The instanced sphere
        Entity* instancingEntity = createPrimitive("sphere", _material.get());
        instancingEntity->setName("InstancingEntity");
        if (auto* render = instancingEntity->findComponent<RenderComponent>(); render && !render->meshInstances().empty()) {
            _instancedMesh = render->meshInstances()[0];
        }

        // A non-instanced sphere with the same material, which the instances crowd onto
        _sphere = createPrimitive("sphere", _material.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f));
        _sphere->setName("sphere");

        updateInstances(Vector3(0.0f, 0.0f, 0.0f));
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // Move the large sphere up and down
        const Vector3 spherePosition(0.0f, std::sin(_time) * 2.0f, 0.0f);
        _sphere->setLocalPosition(spherePosition);

        // Move the instances relative to it
        updateInstances(spherePosition);

        // Orbit the camera
        _camera->setLocalPosition(8.0f * std::sin(_time * 0.1f), 0.0f, 8.0f * std::cos(_time * 0.1f));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
    }

private:
    // Every instance's matrix: its position pulled toward `center` by exp(-0.2 d) of the
    // distance d between them, and its own uniform scale
    void updateInstances(const Vector3& center)
    {
        if (!_instancedMesh) {
            return;
        }
        for (size_t i = 0; i < _instances.size(); ++i) {
            const InstanceData& instance = _instances[i];
            const Vector3 direction = instance.position - center;
            const float displacement = std::exp(-direction.length() * 0.2f);
            const Vector3 worldPosition = instance.position - direction * displacement;
            const Matrix4 matrix = Matrix4::trs(worldPosition, Quaternion(0.0f, 0.0f, 0.0f, 1.0f),
                Vector3(instance.scale, instance.scale, instance.scale));
            matrix.store(&_matrices[i * 16]);
        }

        _bufferIndex = (_bufferIndex + 1) % _instanceBuffers.size();
        const auto& buffer = _instanceBuffers[_bufferIndex];
        buffer->writeRange(0, _matrices.data(), _matrices.size() * sizeof(float));
        _instancedMesh->setInstancing(buffer, kInstanceCount);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::shared_ptr<StandardMaterial> _material;
    std::vector<InstanceData> _instances;
    std::vector<float> _matrices;
    std::vector<std::shared_ptr<VertexBuffer>> _instanceBuffers;
    size_t _bufferIndex = 0;
    MeshInstance* _instancedMesh = nullptr;
    Entity* _sphere = nullptr;
    Entity* _camera = nullptr;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(InstancingCustomExample)
