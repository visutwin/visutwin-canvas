// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream graphics-advanced/multi-draw.
//
// One mesh drawn as many sub-draws (MeshInstance::setMultiDraw). A 120 x 120 terrain is built as a
// grid of 40 x 40 patches, each a 32 x 32-segment plane displaced by the canyon heightmap's red
// channel (-50 to 50) and textured with the canyon diffuse map; all the patches go into one mesh,
// recording where each patch's indices start and how many it has. Every frame a band two patches
// wide sweeps round the centre of the grid (a tenth of a turn a second), and the patches under it
// are left out: the visible patches are packed into the front of the sub-draw list, which then
// draws only those. Under the table mountain environment (skybox mip 2, exposure 1, ambient 0.1);
// the camera at (0, 150, 80) looks at the origin through orbit controls and tone-maps ACES.
//
// DEVIATION: each sub-draw is a draw call of its own (DrawCommands), where upstream issues one
// multi-draw call.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "scene/geometry/geometryUtils.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    // Terrain params
    constexpr float kTerrainWidth = 120.0f;
    constexpr float kTerrainDepth = 120.0f;
    constexpr float kMinHeight = -50.0f;
    constexpr float kMaxHeight = 50.0f;
    constexpr int kPatchesX = 40;
    constexpr int kPatchesZ = 40;
    constexpr int kPatchSegments = 32;   // segments per side for each patch

    constexpr float kBandRadius = 1.0f;  // half-width in grid units (~2-entry band)
    constexpr float kRotRps = 0.1f;      // revolutions per second for the spinning line
}

class MultiDrawExample final : public ExampleApp
{
public:
    MultiDrawExample() : ExampleApp({.title = "Multi Draw"}) {}

protected:
    bool create() override
    {
        _envAsset = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/table-mountain-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _heightAsset = std::make_unique<Asset>("height", AssetType::TEXTURE,
            assetPath("textures/terrain/Canyon-Height.jpg"), AssetData{.mipmaps = false});
        _diffuseAsset = std::make_unique<Asset>("diffuse", AssetType::TEXTURE,
            assetPath("textures/terrain/Canyon-Diffuse.jpg"));
        Texture* env = _envAsset->resourceAs<Texture>();
        Texture* height = _heightAsset->resourceAs<Texture>();
        Texture* diffuse = _diffuseAsset->resourceAs<Texture>();
        const auto* pixels = height ? static_cast<const uint8_t*>(height->getLevel(0)) : nullptr;
        if (!env || !height || !diffuse || !pixels || height->format() != PixelFormat::PIXELFORMAT_RGBA8) {
            spdlog::error("multi-draw needs the table mountain atlas and the canyon height and diffuse maps");
            return false;
        }

        // Setup skydome
        scene()->setSkyboxMip(2);
        scene()->setExposure(1.0f);
        scene()->setEnvAtlas(env);
        scene()->setAmbientLight(0.1f, 0.1f, 0.1f);

        // Camera
        Entity* camera = createCamera(Vector3(0.0f, 150.0f, 80.0f));
        camera->findComponent<CameraComponent>()->setToneMapping(TONEMAP_ACES);
        camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        addOrbitControls(camera)->setEnableFly(false);

        // Material
        _material = std::make_shared<StandardMaterial>();
        _material->setDiffuseMap(diffuse);

        // Heightmap buffer: a height from a global (x, z) in world units, stored in the R channel
        const int bufferWidth = static_cast<int>(height->width());
        const int bufferHeight = static_cast<int>(height->height());
        const auto sampleHeight = [&](const float x, const float z) {
            const float u = (x + kTerrainWidth * 0.5f) / kTerrainWidth;
            const float v = 1.0f - (z + kTerrainDepth * 0.5f) / kTerrainDepth;
            const int ix = std::clamp(static_cast<int>(u * static_cast<float>(bufferWidth - 1)), 0, bufferWidth - 1);
            const int iy = std::clamp(static_cast<int>(v * static_cast<float>(bufferHeight - 1)), 0, bufferHeight - 1);
            const float r = static_cast<float>(pixels[(ix + iy * bufferWidth) * 4]) / 255.0f;
            return kMinHeight + (kMaxHeight - kMinHeight) * r;
        };

        // The reusable patch geometry: a plane centred on the origin of the patch's size
        const float patchWidth = kTerrainWidth / kPatchesX;
        const float patchDepth = kTerrainDepth / kPatchesZ;
        std::vector<float> patchPositions;
        std::vector<uint32_t> patchIndices;
        uint32_t vertexCounter = 0;
        for (int i = 0; i <= kPatchSegments; ++i) {
            for (int j = 0; j <= kPatchSegments; ++j) {
                const float x = -patchWidth * 0.5f + patchWidth * static_cast<float>(i) / kPatchSegments;
                const float z = -(-patchDepth * 0.5f + patchDepth * static_cast<float>(j) / kPatchSegments);
                patchPositions.insert(patchPositions.end(), {x, 0.0f, z});
                if (i < kPatchSegments && j < kPatchSegments) {
                    patchIndices.insert(patchIndices.end(), {
                        vertexCounter + kPatchSegments + 1, vertexCounter + 1, vertexCounter,
                        vertexCounter + kPatchSegments + 1, vertexCounter + kPatchSegments + 2, vertexCounter + 1});
                }
                ++vertexCounter;
            }
        }

        // The combined mesh, built from the grid of patches
        PrimitiveGeometry geometry;
        uint32_t vertexBase = 0;
        for (int pz = 0; pz < kPatchesZ; ++pz) {
            for (int px = 0; px < kPatchesX; ++px) {
                const float centerX = -kTerrainWidth * 0.5f + (static_cast<float>(px) + 0.5f) * patchWidth;
                const float centerZ = -kTerrainDepth * 0.5f + (static_cast<float>(pz) + 0.5f) * patchDepth;

                // The first index of this patch
                _firstIndexPerPatch.push_back(static_cast<int>(geometry.indices.size()));

                for (size_t i = 0; i < patchPositions.size(); i += 3) {
                    const float wx = patchPositions[i] + centerX;
                    const float wz = patchPositions[i + 2] + centerZ;
                    geometry.positions.insert(geometry.positions.end(), {wx, sampleHeight(wx, wz), wz});
                    geometry.uvs.insert(geometry.uvs.end(), {(wx + kTerrainWidth * 0.5f) / kTerrainWidth,
                        1.0f - (wz + kTerrainDepth * 0.5f) / kTerrainDepth});
                }
                for (const uint32_t index : patchIndices) {
                    geometry.indices.push_back(vertexBase + index);
                }
                _indexCountPerPatch.push_back(static_cast<int>(patchIndices.size()));
                vertexBase += static_cast<uint32_t>(patchPositions.size() / 3);
            }
        }

        // Normals after displacement
        geometry.normals = calculateNormals(geometry.positions, geometry.indices);
        _mesh = createMeshFromGeometry(device(), geometry);

        // The entity drawing the mesh instance
        auto* entity = new Entity();
        entity->setName("TerrainEntity");
        entity->setEngine(engine());
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        auto meshInstance = std::make_unique<MeshInstance>(_mesh.get(), _material.get(), entity);
        // The multi-draw list: one sub-draw per patch
        _commands = meshInstance->setMultiDraw(nullptr, kPatchesX * kPatchesZ);
        render->addMeshInstance(std::move(meshInstance));
        root()->addChild(entity);
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // The spinning band: an infinite line through the grid centre at angle theta; the patches
        // within the band's half-width of it are hidden
        constexpr float cx = (kPatchesX - 1) * 0.5f;
        constexpr float cz = (kPatchesZ - 1) * 0.5f;
        const float theta = _time * kRotRps * std::numbers::pi_v<float> * 2.0f;
        const float s = std::sin(theta);
        const float c = std::cos(theta);

        // Pack the visible draws into the front of the list, hiding the patches in the band
        int write = 0;
        for (int pz = 0; pz < kPatchesZ; ++pz) {
            for (int px = 0; px < kPatchesX; ++px) {
                // The perpendicular distance in grid units to the line
                const float dx = static_cast<float>(px) - cx;
                const float dz = static_cast<float>(pz) - cz;
                if (std::abs(dx * s - dz * c) <= kBandRadius) {
                    continue;
                }
                const int index = pz * kPatchesX + px;
                _commands->add(write, _indexCountPerPatch[index], 1, _firstIndexPerPatch[index], 0, 0);
                ++write;
            }
        }
        _commands->update(write);
    }

private:
    std::unique_ptr<Asset> _envAsset;
    std::unique_ptr<Asset> _heightAsset;
    std::unique_ptr<Asset> _diffuseAsset;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<Mesh> _mesh;
    DrawCommands* _commands = nullptr;
    std::vector<int> _firstIndexPerPatch;
    std::vector<int> _indexCountPerPatch;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MultiDrawExample)
