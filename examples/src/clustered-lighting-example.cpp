// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Clustered lighting example — port of upstream graphics/clustered-lighting.
//
// A high-polycount cylinder stands on a large normal-mapped ground plane. 30 omni
// lights ride a sine wave around the cylinder and 16 spot lights orbit at its base,
// all with emissive marker geometry, plus one rotating shadow-casting directional
// light. With Scene::setClusteredLightingEnabled(true) the unshadowed local lights
// are bucketed into a 3D world-space grid (WorldClusters) so all 46 are evaluated
// in a single forward pass.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"

using namespace visutwin::canvas;

// DEVIATION: RenderComponent's built-in "cylinder" primitive is fixed at 20 sides.
// Upstream builds the cylinder from CylinderGeometry({ capSegments: 200 }), so the
// high-poly mesh is generated here instead (a 20-sided cylinder at this scale reads
// as a faceted prism under 30 moving lights).
std::shared_ptr<Mesh> createHighPolyCylinder(const std::shared_ptr<GraphicsDevice>& device, int sides)
{
    constexpr float PI_F = 3.14159265358979323846f;
    constexpr float radius = 0.5f;
    constexpr float halfHeight = 0.5f;

    // Interleaved standard vertex: position(3) normal(3) uv0(2) tangent(4) uv1(2) = 14 floats.
    std::vector<float> vertices;
    std::vector<uint32_t> indices;

    const auto pushVertex = [&](const Vector3& p, const Vector3& n, const Vector3& t,
                                float u, float v) {
        vertices.insert(vertices.end(), {
            p.getX(), p.getY(), p.getZ(),
            n.getX(), n.getY(), n.getZ(),
            u, v,
            t.getX(), t.getY(), t.getZ(), 1.0f,
            u, v
        });
    };

    // --- Side wall (seam vertex duplicated so UVs wrap cleanly) ---
    for (int i = 0; i <= sides; ++i) {
        const float theta = static_cast<float>(i) / static_cast<float>(sides) * 2.0f * PI_F;
        const float c = std::cos(theta);
        const float s = std::sin(theta);
        const Vector3 n(c, 0.0f, s);
        const Vector3 t(-s, 0.0f, c);
        const float u = static_cast<float>(i) / static_cast<float>(sides);
        pushVertex(Vector3(radius * c, -halfHeight, radius * s), n, t, u, 0.0f);
        pushVertex(Vector3(radius * c, halfHeight, radius * s), n, t, u, 1.0f);
    }
    for (int i = 0; i < sides; ++i) {
        const uint32_t b0 = static_cast<uint32_t>(i * 2);
        const uint32_t t0 = b0 + 1u;
        const uint32_t b1 = b0 + 2u;
        const uint32_t t1 = b0 + 3u;
        indices.insert(indices.end(), {b0, t0, b1, b1, t0, t1});
    }

    // --- Caps ---
    const auto addCap = [&](float y, float ny) {
        const uint32_t center = static_cast<uint32_t>(vertices.size() / 14u);
        const Vector3 n(0.0f, ny, 0.0f);
        const Vector3 t(1.0f, 0.0f, 0.0f);
        pushVertex(Vector3(0.0f, y, 0.0f), n, t, 0.5f, 0.5f);
        for (int i = 0; i <= sides; ++i) {
            const float theta = static_cast<float>(i) / static_cast<float>(sides) * 2.0f * PI_F;
            const float c = std::cos(theta);
            const float s = std::sin(theta);
            pushVertex(Vector3(radius * c, y, radius * s), n, t,
                       0.5f + 0.5f * c, 0.5f + 0.5f * s);
        }
        for (int i = 0; i < sides; ++i) {
            const uint32_t r0 = center + 1u + static_cast<uint32_t>(i);
            const uint32_t r1 = r0 + 1u;
            if (ny > 0.0f) {
                indices.insert(indices.end(), {center, r1, r0});
            } else {
                indices.insert(indices.end(), {center, r0, r1});
            }
        }
    };
    addCap(halfHeight, 1.0f);
    addCap(-halfHeight, -1.0f);

    const int vertexCount = static_cast<int>(vertices.size() / 14u);

    auto vertexFormat = std::make_shared<VertexFormat>(
        56, VertexFormat::standardElements(), true, false);

    VertexBufferOptions vbOptions;
    vbOptions.data.resize(vertices.size() * sizeof(float));
    std::memcpy(vbOptions.data.data(), vertices.data(), vbOptions.data.size());
    auto vertexBuffer = device->createVertexBuffer(vertexFormat, vertexCount, vbOptions);

    std::vector<uint8_t> indexBytes(indices.size() * sizeof(uint32_t));
    std::memcpy(indexBytes.data(), indices.data(), indexBytes.size());
    auto indexBuffer = device->createIndexBuffer(
        INDEXFORMAT_UINT32, static_cast<int>(indices.size()), indexBytes);

    auto mesh = std::make_shared<Mesh>();
    mesh->setVertexBuffer(vertexBuffer);
    mesh->setIndexBuffer(indexBuffer);
    Primitive prim;
    prim.type = PRIMITIVE_TRIANGLES;
    prim.indexed = true;
    prim.base = 0;
    prim.count = static_cast<int>(indices.size());
    mesh->setPrimitive(prim);
    mesh->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f),
                              Vector3(radius, halfHeight, radius)));
    return mesh;
}

class ClusteredLightingExample final: public ExampleApp
{
public:
    ClusteredLightingExample()
        : ExampleApp({.title = "Clustered Lighting", .width = 1100, .height = 750}) {}

protected:
    bool create() override
    {
        // Enable clustered lighting: the unshadowed local lights are bucketed into a 3D
        // world-space grid so many of them can be evaluated cheaply in one pass.
        scene()->setClusteredLightingEnabled(true);
        // The cluster grid defaults already match upstream's tuning for this scene
        // (ClusterConfig: 12x16x12 cells, 48 lights per cell).
        scene()->setToneMapping(TONEMAP_ACES);

        // Tiling normal map shared by the ground plane and the cylinder (upstream uses the
        // same material for both).
        // NOTE: AssetData::mipmaps defaults to false here (upstream defaults to true) — without
        // mips the tiled normal map aliases into per-pixel noise across the 150-unit ground.
        _normalMapAsset = std::make_unique<Asset>(
            "normal-map",
            AssetType::TEXTURE,
            assetPath("textures/normal-map.png"),
            AssetData{ .mipmaps = true }
        );
        const auto normalMapResource = _normalMapAsset->resource();
        if (!normalMapResource) {
            spdlog::error("Failed to load textures/normal-map.png");
            return false;
        }
        auto* normalMap = std::get<Texture*>(*normalMapResource);

        // --- Shared material: tiled normal map, mildly glossy metal ---
        _material = std::make_shared<StandardMaterial>();
        _material->setNormalMap(normalMap);
        _material->setNormalMapTiling(Vector2(5.0f, 5.0f));
        _material->setBumpiness(1.0f);
        _material->setGloss(0.5f);
        _material->setMetalness(0.3f);

        // --- Ground plane ---
        auto* ground = createPrimitive("plane", _material.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(150.0f, 150.0f, 150.0f));
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(true);
        }

        // --- High polycount cylinder ---
        _cylinderMesh = createHighPolyCylinder(device(), 200);
        auto* cylinder = new Entity();
        cylinder->setEngine(engine());
        cylinder->setLocalPosition(0.0f, 50.0f, 0.0f);
        cylinder->setLocalScale(50.0f, 100.0f, 50.0f);
        if (auto* render = static_cast<RenderComponent*>(cylinder->addComponent<RenderComponent>())) {
            render->setMaterial(_material.get());
            auto meshInstance = std::make_unique<MeshInstance>(
                _cylinderMesh.get(), _material.get(), cylinder);
            meshInstance->setCastShadow(true);
            meshInstance->setReceiveShadow(true);
            render->setCastShadows(true);
            render->setReceiveShadows(true);
            render->addMeshInstance(std::move(meshInstance));
        }
        root()->addChild(cylinder);

        std::mt19937 rng(1234u);
        std::uniform_real_distribution<float> rand01(0.0f, 1.0f);

        // --- 30 omni lights that do not cast shadows, each with an emissive sphere ---
        for (int i = 0; i < 30; ++i) {
            const Color color(rand01(rng), rand01(rng), rand01(rng), 1.0f);

            auto* lightPoint = new Entity();
            lightPoint->setEngine(engine());
            if (auto* lc = static_cast<LightComponent*>(lightPoint->addComponent<LightComponent>())) {
                lc->setType(LightType::LIGHTTYPE_OMNI);
                lc->setColor(color);
                lc->setIntensity(2.0f);
                lc->setRange(12.0f);
                lc->setCastShadows(false);
                lc->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
            }

            auto markerMaterial = std::make_shared<StandardMaterial>();
            markerMaterial->setEmissive(color);
            markerMaterial->setEmissiveIntensity(10.0f);
            _markerMaterials.push_back(markerMaterial);

            if (auto* render = static_cast<RenderComponent*>(lightPoint->addComponent<RenderComponent>())) {
                render->setMaterial(markerMaterial.get());
                render->setType("sphere");
                render->setCastShadows(true);
            }
            lightPoint->setLocalScale(5.0f, 5.0f, 5.0f);

            root()->addChild(lightPoint);
            _pointLights.push_back(lightPoint);
        }

        // --- 16 spot lights, each with an emissive cone ---
        for (int i = 0; i < 16; ++i) {
            const Color color(rand01(rng), rand01(rng), rand01(rng), 1.0f);

            auto* lightSpot = new Entity();
            lightSpot->setEngine(engine());
            if (auto* lc = static_cast<LightComponent*>(lightSpot->addComponent<LightComponent>())) {
                lc->setType(LightType::LIGHTTYPE_SPOT);
                lc->setColor(color);
                lc->setIntensity(2.0f);
                lc->setInnerConeAngle(5.0f);
                lc->setOuterConeAngle(6.0f + rand01(rng) * 40.0f);
                lc->setRange(25.0f);
                lc->setCastShadows(false);
            }

            auto markerMaterial = std::make_shared<StandardMaterial>();
            markerMaterial->setEmissive(color);
            markerMaterial->setEmissiveIntensity(10.0f);
            _markerMaterials.push_back(markerMaterial);

            if (auto* render = static_cast<RenderComponent*>(lightSpot->addComponent<RenderComponent>())) {
                render->setMaterial(markerMaterial.get());
                render->setType("cone");
            }
            lightSpot->setLocalScale(5.0f, 5.0f, 5.0f);

            lightSpot->setLocalPosition(100.0f, 50.0f, 70.0f);
            lightSpot->lookAt(Vector3(100.0f, 60.0f, 70.0f));
            root()->addChild(lightSpot);
            _spotLights.push_back(lightSpot);
        }

        // --- A single shadow-casting directional light ---
        _dirLight = createDirectionalLight(Vector3(0.0f, 0.0f, 0.0f), Color(1.0f, 1.0f, 1.0f), 0.15f, true);
        if (auto* lc = _dirLight->findComponent<LightComponent>()) {
            lc->setRange(300.0f);
            lc->setShadowDistance(600.0f);
            lc->setShadowBias(0.2f);
            lc->setShadowNormalBias(0.05f);
        }

        // --- Camera ---
        auto* camera = createCamera(Vector3(140.0f, 140.0f, 140.0f));
        if (auto* cameraComp = camera->findComponent<CameraComponent>();
            cameraComp && cameraComp->camera()) {
            cameraComp->camera()->setClearColor(Color(0.05f, 0.05f, 0.05f, 1.0f));
            cameraComp->camera()->setNearClip(0.1f);
            cameraComp->camera()->setFarClip(500.0f);
        }

        // addOrbitControls derives the orbit distance and angles from the current camera
        // position, so the upstream pose is preserved exactly.
        auto* cameraControls = addOrbitControls(camera, Vector3(0.0f, 40.0f, 0.0f));
        cameraControls->setMoveSpeed(60.0f);
        cameraControls->storeResetState();

        spdlog::info("*** Clustered Lighting Example ***");
        spdlog::info("{} omni + {} spot clustered lights, 1 shadow-casting directional.",
                     _pointLights.size(), _spotLights.size());
        spdlog::info("Orbit: LMB/RMB, Wheel zoom, R reset, Esc quit.");

        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        const float time = _time;

        // Move the omni lights along sine-based waves around the cylinder.
        for (size_t i = 0; i < _pointLights.size(); ++i) {
            const float angle = static_cast<float>(i) / static_cast<float>(_pointLights.size()) *
                                2.0f * 3.14159265358979323846f;
            const float y = std::sin(time * 0.5f + 7.0f * angle) * 30.0f + 70.0f;
            _pointLights[i]->setLocalPosition(30.0f * std::sin(angle), y, 30.0f * std::cos(angle));
        }

        // Rotate the spot lights around the base of the cylinder, aimed at its centre.
        for (size_t i = 0; i < _spotLights.size(); ++i) {
            const float angle = static_cast<float>(i) / static_cast<float>(_spotLights.size()) *
                                2.0f * 3.14159265358979323846f;
            _spotLights[i]->setLocalPosition(40.0f * std::sin(time + angle), 5.0f,
                                             40.0f * std::cos(time + angle));
            _spotLights[i]->lookAt(Vector3(0.0f, 0.0f, 0.0f));
            // Lights emit along their local -Y, so tilt the aimed -Z axis onto it.
            _spotLights[i]->rotateLocal(90.0f, 0.0f, 0.0f);
        }

        // Rotate the directional light.
        _dirLight->setLocalEulerAngles(25.0f, -30.0f * time, 0.0f);
    }

    void destroy() override
    {
        spdlog::info("*** Clustered Lighting Example Finished ***");
    }

private:
    std::unique_ptr<Asset> _normalMapAsset;
    std::shared_ptr<StandardMaterial> _material;
    std::shared_ptr<Mesh> _cylinderMesh;
    // Materials are kept alive for the lifetime of the app.
    std::vector<std::shared_ptr<StandardMaterial>> _markerMaterials;

    std::vector<Entity*> _pointLights;
    std::vector<Entity*> _spotLights;
    Entity* _dirLight = nullptr;

    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(ClusteredLightingExample)
