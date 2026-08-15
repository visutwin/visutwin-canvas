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
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"

constexpr int WINDOW_WIDTH = 1100;
constexpr int WINDOW_HEIGHT = 750;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

// Tiling normal map shared by the ground plane and the cylinder (upstream uses the
// same material for both).
// NOTE: AssetData::mipmaps defaults to false here (upstream defaults to true) — without
// mips the tiled normal map aliases into per-pixel noise across the 150-unit ground.
const auto normalMapAsset = std::make_unique<Asset>(
    "normal-map",
    AssetType::TEXTURE,
    rootPath + "/textures/normal-map.png",
    AssetData{ .mipmaps = true }
);

// Aim an entity's -Z axis at a target (equivalent of upstream Entity::lookAt).
void setLookAt(Entity* entity, const Vector3& target)
{
    if (!entity) {
        return;
    }

    const Vector3 position = entity->position();
    const Vector3 dir = (target - position).normalized();
    const float pitchDeg = std::asin(std::clamp(dir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yawDeg = std::atan2(-dir.getX(), -dir.getZ()) * RAD_TO_DEG;
    entity->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
}

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

int main()
{
    log::init();
    log::set_level_debug();

    window = nullptr;
    renderer = nullptr;

    const auto shutdown = []() {
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Clustered Lighting", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!window) { shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { shutdown(); return -1; }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) { shutdown(); return -1; }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();

    // Enable clustered lighting: the unshadowed local lights are bucketed into a 3D
    // world-space grid so many of them can be evaluated cheaply in one pass.
    scene->setClusteredLightingEnabled(true);
    // The cluster grid defaults already match upstream's tuning for this scene
    // (ClusterConfig: 12x16x12 cells, 48 lights per cell).
    scene->setToneMapping(TONEMAP_ACES);

    const auto normalMapResource = normalMapAsset->resource();
    if (!normalMapResource) {
        spdlog::error("Failed to load textures/normal-map.png");
        shutdown();
        return -1;
    }
    auto* normalMap = std::get<Texture*>(*normalMapResource);

    // --- Shared material: tiled normal map, mildly glossy metal ---
    auto material = std::make_shared<StandardMaterial>();
    material->setNormalMap(normalMap);
    material->setNormalMapTiling(Vector2(5.0f, 5.0f));
    material->setBumpiness(1.0f);
    material->setGloss(0.5f);
    material->setMetalness(0.3f);

    // --- Ground plane ---
    auto* ground = new Entity();
    ground->setEngine(engine.get());
    ground->setLocalScale(150.0f, 150.0f, 150.0f);
    if (auto* render = static_cast<RenderComponent*>(ground->addComponent<RenderComponent>())) {
        render->setMaterial(material.get());
        render->setType("plane");
        render->setCastShadows(false);
        render->setReceiveShadows(true);
    }
    engine->root()->addChild(ground);

    // --- High polycount cylinder ---
    auto cylinderMesh = createHighPolyCylinder(graphicsDevice, 200);
    auto* cylinder = new Entity();
    cylinder->setEngine(engine.get());
    cylinder->setLocalPosition(0.0f, 50.0f, 0.0f);
    cylinder->setLocalScale(50.0f, 100.0f, 50.0f);
    if (auto* render = static_cast<RenderComponent*>(cylinder->addComponent<RenderComponent>())) {
        render->setMaterial(material.get());
        auto meshInstance = std::make_unique<MeshInstance>(
            cylinderMesh.get(), material.get(), cylinder);
        meshInstance->setCastShadow(true);
        meshInstance->setReceiveShadow(true);
        render->setCastShadows(true);
        render->setReceiveShadows(true);
        render->addMeshInstance(std::move(meshInstance));
    }
    engine->root()->addChild(cylinder);

    std::mt19937 rng(1234u);
    std::uniform_real_distribution<float> rand01(0.0f, 1.0f);

    // Materials are kept alive for the lifetime of the app.
    std::vector<std::shared_ptr<StandardMaterial>> markerMaterials;

    // --- 30 omni lights that do not cast shadows, each with an emissive sphere ---
    std::vector<Entity*> pointLights;
    for (int i = 0; i < 30; ++i) {
        const Color color(rand01(rng), rand01(rng), rand01(rng), 1.0f);

        auto* lightPoint = new Entity();
        lightPoint->setEngine(engine.get());
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
        markerMaterials.push_back(markerMaterial);

        if (auto* render = static_cast<RenderComponent*>(lightPoint->addComponent<RenderComponent>())) {
            render->setMaterial(markerMaterial.get());
            render->setType("sphere");
            render->setCastShadows(true);
        }
        lightPoint->setLocalScale(5.0f, 5.0f, 5.0f);

        engine->root()->addChild(lightPoint);
        pointLights.push_back(lightPoint);
    }

    // --- 16 spot lights, each with an emissive cone ---
    std::vector<Entity*> spotLights;
    for (int i = 0; i < 16; ++i) {
        const Color color(rand01(rng), rand01(rng), rand01(rng), 1.0f);

        auto* lightSpot = new Entity();
        lightSpot->setEngine(engine.get());
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
        markerMaterials.push_back(markerMaterial);

        if (auto* render = static_cast<RenderComponent*>(lightSpot->addComponent<RenderComponent>())) {
            render->setMaterial(markerMaterial.get());
            render->setType("cone");
        }
        lightSpot->setLocalScale(5.0f, 5.0f, 5.0f);

        lightSpot->setLocalPosition(100.0f, 50.0f, 70.0f);
        setLookAt(lightSpot, Vector3(100.0f, 60.0f, 70.0f));
        engine->root()->addChild(lightSpot);
        spotLights.push_back(lightSpot);
    }

    // --- A single shadow-casting directional light ---
    auto* dirLight = new Entity();
    dirLight->setEngine(engine.get());
    if (auto* lc = static_cast<LightComponent*>(dirLight->addComponent<LightComponent>())) {
        lc->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        lc->setColor(Color(1.0f, 1.0f, 1.0f));
        lc->setIntensity(0.15f);
        lc->setRange(300.0f);
        lc->setShadowDistance(600.0f);
        lc->setCastShadows(true);
        lc->setShadowBias(0.2f);
        lc->setShadowNormalBias(0.05f);
    }
    engine->root()->addChild(dirLight);

    // --- Camera ---
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setClearColor(Color(0.05f, 0.05f, 0.05f, 1.0f));
        cameraComp->camera()->setNearClip(0.1f);
        cameraComp->camera()->setFarClip(500.0f);
    }
    camera->setLocalPosition(140.0f, 140.0f, 140.0f);
    engine->root()->addChild(camera);

    auto* cameraControls = camera->script()->create<CameraControls>();
    // setFocusPoint derives the orbit distance and angles from the current camera
    // position, so the upstream pose is preserved exactly.
    cameraControls->setFocusPoint(Vector3(0.0f, 40.0f, 0.0f));
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(60.0f);
    cameraControls->storeResetState();

    spdlog::info("*** Clustered Lighting Example ***");
    spdlog::info("{} omni + {} spot clustered lights, 1 shadow-casting directional.",
                 pointLights.size(), spotLights.size());
    spdlog::info("Orbit: LMB/RMB, Wheel zoom, R reset, Esc quit.");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float time = 0.0f;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(static_cast<double>(nowCounter - prevCounter) /
                                            static_cast<double>(perfFreq));
        prevCounter = nowCounter;
        time += dt;

        // Move the omni lights along sine-based waves around the cylinder.
        for (size_t i = 0; i < pointLights.size(); ++i) {
            const float angle = static_cast<float>(i) / static_cast<float>(pointLights.size()) *
                                2.0f * 3.14159265358979323846f;
            const float y = std::sin(time * 0.5f + 7.0f * angle) * 30.0f + 70.0f;
            pointLights[i]->setLocalPosition(30.0f * std::sin(angle), y, 30.0f * std::cos(angle));
        }

        // Rotate the spot lights around the base of the cylinder, aimed at its centre.
        for (size_t i = 0; i < spotLights.size(); ++i) {
            const float angle = static_cast<float>(i) / static_cast<float>(spotLights.size()) *
                                2.0f * 3.14159265358979323846f;
            spotLights[i]->setLocalPosition(40.0f * std::sin(time + angle), 5.0f,
                                            40.0f * std::cos(time + angle));
            setLookAt(spotLights[i], Vector3(0.0f, 0.0f, 0.0f));
            // Lights emit along their local -Y, so tilt the aimed -Z axis onto it.
            spotLights[i]->rotateLocal(90.0f, 0.0f, 0.0f);
        }

        // Rotate the directional light.
        dirLight->setLocalEulerAngles(25.0f, -30.0f * time, 0.0f);

        engine->update(dt);
        engine->render();
    }

    shutdown();
    spdlog::info("*** Clustered Lighting Example Finished ***");
    return 0;
}
