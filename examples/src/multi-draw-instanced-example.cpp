// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream graphics/multi-draw-instanced.
//
// Sub-draws of one instanced mesh, each with its own instance range. A sphere (radius 0.5, 24 x 24
// bands), a unit box and a cylinder (radius 0.5, height 1, 32 cap segments) go into one mesh, and
// one instance buffer holds three rings of instance matrices: 8 at radius 2, 15 at radius 4 and 25 at
// radius 6, each instance turned to face along its ring. Three sub-draws (MeshInstance::
// setMultiDraw) draw the sphere on the first ring, the box on the second and the cylinder on the
// third, each from its first instance on. Coloured lines (red, green, yellow) trace the rings. The
// material is a metal (gloss 0.6, metalness 0.7) under the helipad environment (skybox mip 2,
// exposure 0.3, ambient 0.1); the camera orbits the origin at 15 units, 7 up, 0.2 radians a second,
// and tone-maps ACES.
//
// DEVIATION: each sub-draw is a draw call of its own (DrawCommands), where upstream issues one
// multi-draw call.
//
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr std::array<int, 3> kRingCounts{8, 15, 25};
    constexpr std::array<float, 3> kRadii{2.0f, 4.0f, 6.0f};
}

class MultiDrawInstancedExample final : public ExampleApp
{
public:
    MultiDrawInstancedExample() : ExampleApp({.title = "Multi Draw Instanced"}) {}

protected:
    bool create() override
    {
        _helipadAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* helipad = _helipadAsset->resourceAs<Texture>();
        if (!helipad) {
            spdlog::error("multi-draw-instanced needs the helipad env atlas");
            return false;
        }

        // Setup skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(0.3f);
        scene()->setEnvAtlas(helipad);
        scene()->setAmbientLight(0.1f, 0.1f, 0.1f);

        // Camera
        _camera = createCamera(Vector3(0.0f, 0.0f, 16.0f));
        _camera->findComponent<CameraComponent>()->setToneMapping(TONEMAP_ACES);

        // Material
        _material = std::make_shared<StandardMaterial>();
        _material->setGloss(0.6f);
        _material->setMetalness(0.7f);
        _material->setUseMetalness(true);

        // The three primitive geometries (unit size), combined into one
        const std::array<PrimitiveGeometry, 3> parts{createSphereGeometry(0.5f, 24, 24), createBoxGeometry(),
            createConeBaseGeometry(0.5f, 0.5f, 1.0f, 1, 32, false)};
        PrimitiveGeometry combined;
        std::array<int, 3> indexCounts{};
        std::array<int, 3> firstIndex{};
        uint32_t vertexOffset = 0;
        for (size_t p = 0; p < parts.size(); ++p) {
            const PrimitiveGeometry& part = parts[p];
            firstIndex[p] = static_cast<int>(combined.indices.size());
            indexCounts[p] = static_cast<int>(part.indices.size());
            combined.positions.insert(combined.positions.end(), part.positions.begin(), part.positions.end());
            combined.normals.insert(combined.normals.end(), part.normals.begin(), part.normals.end());
            combined.uvs.insert(combined.uvs.end(), part.uvs.begin(), part.uvs.end());
            combined.tangents.insert(combined.tangents.end(), part.tangents.begin(), part.tangents.end());
            for (const uint32_t index : part.indices) {
                combined.indices.push_back(index + vertexOffset);
            }
            vertexOffset += static_cast<uint32_t>(part.positions.size() / 3);
        }
        _mesh = createMeshFromGeometry(device(), combined);

        // The entity drawing the mesh instance
        auto* entity = new Entity();
        entity->setName("MultiDrawEntity");
        entity->setEngine(engine());
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        auto meshInstance = std::make_unique<MeshInstance>(_mesh.get(), _material.get(), entity);

        // The instance matrices on three concentric rings, one ring after another
        const int totalInstances = kRingCounts[0] + kRingCounts[1] + kRingCounts[2];
        constexpr int kInstanceBytes = VertexFormat::INSTANCING_MATRIX_SIZE;
        std::vector<uint8_t> instanceBytes(static_cast<size_t>(totalInstances) * kInstanceBytes);
        int write = 0;
        for (size_t ring = 0; ring < kRingCounts.size(); ++ring) {
            const int n = kRingCounts[ring];
            for (int i = 0; i < n; ++i) {
                const float a = static_cast<float>(i) / static_cast<float>(n) * std::numbers::pi_v<float> * 2.0f;
                const Matrix4 matrix = Matrix4::trs(Vector3(std::cos(a) * kRadii[ring], 0.0f,
                    std::sin(a) * kRadii[ring]), Quaternion::fromEulerAngles(0.0f, a * 180.0f / std::numbers::pi_v<float>,
                    0.0f), Vector3(1.0f, 1.0f, 1.0f));
                std::memcpy(instanceBytes.data() + static_cast<size_t>(write) * kInstanceBytes, &matrix,
                    kInstanceBytes);
                ++write;
            }
        }
        VertexBufferOptions options;
        options.data = std::move(instanceBytes);
        _instances = device()->createVertexBuffer(VertexFormat::defaultInstancingFormat(), totalInstances, options);
        meshInstance->setInstancing(_instances, totalInstances);

        // The multi-draw list: the sphere, the box and the cylinder, each on its own ring
        DrawCommands* commands = meshInstance->setMultiDraw(nullptr, 3);
        int firstInstance = 0;
        for (int p = 0; p < 3; ++p) {
            commands->add(p, indexCounts[p], kRingCounts[p], firstIndex[p], 0, firstInstance);
            firstInstance += kRingCounts[p];
        }
        commands->update(3);

        render->addMeshInstance(std::move(meshInstance));
        root()->addChild(entity);
        return true;
    }

    void update(const float dt) override
    {
        // Orbit camera
        _angle += dt * 0.2f;
        _camera->setLocalPosition(15.0f * std::sin(_angle), 7.0f, 15.0f * std::cos(_angle));
        _camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // The lines round each ring, to show how the instances are spread
        const std::array<Color, 3> ringColors{Color(1.0f, 0.0f, 0.0f, 1.0f), Color(0.0f, 1.0f, 0.0f, 1.0f),
            Color(1.0f, 1.0f, 0.0f, 1.0f)};
        std::vector<Vector3> positions;
        std::vector<Color> colors;
        for (size_t ring = 0; ring < kRingCounts.size(); ++ring) {
            const int n = kRingCounts[ring];
            const float r = kRadii[ring];
            for (int i = 0; i < n; ++i) {
                const float a0 = static_cast<float>(i) / static_cast<float>(n) * std::numbers::pi_v<float> * 2.0f;
                const float a1 = static_cast<float>((i + 1) % n) / static_cast<float>(n) *
                    std::numbers::pi_v<float> * 2.0f;
                positions.emplace_back(std::cos(a0) * r, 0.0f, std::sin(a0) * r);
                positions.emplace_back(std::cos(a1) * r, 0.0f, std::sin(a1) * r);
                colors.push_back(ringColors[ring]);
                colors.push_back(ringColors[ring]);
            }
        }
        scene()->drawLines(positions, colors);
    }

private:
    std::unique_ptr<Asset> _helipadAsset;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<Mesh> _mesh;
    std::shared_ptr<VertexBuffer> _instances;
    Entity* _camera = nullptr;
    float _angle = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MultiDrawInstancedExample)
