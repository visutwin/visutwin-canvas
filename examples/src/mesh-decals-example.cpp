// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Mesh decals demo — port of PlayCanvas examples/src/examples/graphics/mesh-decals.example.mjs.
//
// PlayCanvas does not have a dedicated decal subsystem; "mesh decals" is a *technique*
// using StandardMaterial primitives:
//   • dynamic flat-quad mesh built incrementally as decals are stamped
//   • additive-alpha blend so overlapping stamps brighten
//   • depthBias / slopeDepthBias polygon offset to avoid z-fighting with the host surface
//   • depthWrite disabled (the host surface owns depth)
//   • per-vertex color tinting + diffuse texture cutout (heart silhouette)
//
// The bouncing ball drops one decal each time it crosses the ground plane; older decals
// gradually fade as their vertex colors are scaled toward zero.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

// 500 decals × 4 verts/quad. Position+normal+uv0+tangent+uv1+color = 18 floats = 72 bytes
// (matches the STRIDE_WITH_COLOR layout in metalRenderPipeline.cpp).
constexpr int NUM_DECALS = 500;
constexpr int VERTS_PER_DECAL = 4;
constexpr int FLOATS_PER_VERTEX = 18;
constexpr int VERTEX_STRIDE_BYTES = FLOATS_PER_VERTEX * static_cast<int>(sizeof(float));
constexpr int TOTAL_VERTS = NUM_DECALS * VERTS_PER_DECAL;
constexpr int INDICES_PER_DECAL = 6;
constexpr int TOTAL_INDICES = NUM_DECALS * INDICES_PER_DECAL;

using namespace visutwin::canvas;

const std::string rootPath = ASSET_DIR;

const auto helipad = std::make_unique<Asset>(
    "helipad-env-atlas",
    AssetType::TEXTURE,
    rootPath + "/cubemaps/helipad-env-atlas.png",
    AssetData{
        .type = TextureType::TEXTURETYPE_RGBP,
        .mipmaps = false
    }
);

const auto heart = std::make_unique<Asset>(
    "heart",
    AssetType::TEXTURE,
    rootPath + "/textures/heart.png"
);

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

namespace
{
    // Layout of one vertex (matches the engine's STRIDE_WITH_COLOR=72 pipeline path):
    //   [0..2]   position    (float3)
    //   [3..5]   normal      (float3)   — points up for ground decals
    //   [6..7]   uv0         (float2)
    //   [8..11]  tangent     (float4)   — unused by unlit path; filled with sane defaults
    //   [12..13] uv1         (float2)   — unused; mirrored from uv0
    //   [14..17] color       (float4)   — per-vertex tint, alpha = 1
    constexpr int OFF_POS = 0;
    constexpr int OFF_NORMAL = 3;
    constexpr int OFF_UV0 = 6;
    constexpr int OFF_TANGENT = 8;
    constexpr int OFF_UV1 = 12;
    constexpr int OFF_COLOR = 14;

    struct DecalQuad
    {
        Vector3 center;     // world position on ground (y=0 by construction)
        float angleRad = 0; // rotation around +Y
        float size = 1.0f;
        float r = 1, g = 1, b = 1, a = 1;
    };

    // Write the 4 corner vertices for decal `i` into the interleaved float buffer.
    // Quad is laid flat on the y=0 plane, oriented by `angleRad`, scaled by `size`.
    void writeDecal(std::vector<float>& vertices, int decalIndex, const DecalQuad& d)
    {
        // 4 corners, rotated 90° apart. PlayCanvas's example uses the same trick:
        // start at angleRad and step π/2 between each corner; uv pattern is (0,0)(0,1)(1,1)(1,0).
        constexpr float kQuarter = 1.5707963267948966f;
        const float uvs[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};

        for (int corner = 0; corner < 4; ++corner) {
            const float a = d.angleRad + static_cast<float>(corner) * kQuarter;
            const float px = d.center.getX() + d.size * std::sin(a);
            const float py = d.center.getY();    // already on ground
            const float pz = d.center.getZ() + d.size * std::cos(a);

            float* v = &vertices[(decalIndex * VERTS_PER_DECAL + corner) * FLOATS_PER_VERTEX];

            v[OFF_POS + 0] = px;
            v[OFF_POS + 1] = py;
            v[OFF_POS + 2] = pz;

            v[OFF_NORMAL + 0] = 0.0f;
            v[OFF_NORMAL + 1] = 1.0f;
            v[OFF_NORMAL + 2] = 0.0f;

            v[OFF_UV0 + 0] = uvs[corner][0];
            v[OFF_UV0 + 1] = uvs[corner][1];

            // Tangent: harmless default along +X, w = 1 (handedness).
            v[OFF_TANGENT + 0] = 1.0f;
            v[OFF_TANGENT + 1] = 0.0f;
            v[OFF_TANGENT + 2] = 0.0f;
            v[OFF_TANGENT + 3] = 1.0f;

            v[OFF_UV1 + 0] = uvs[corner][0];
            v[OFF_UV1 + 1] = uvs[corner][1];

            v[OFF_COLOR + 0] = d.r;
            v[OFF_COLOR + 1] = d.g;
            v[OFF_COLOR + 2] = d.b;
            // Alpha must be 1 in this engine: VT_FEATURE_VERTEX_COLORS multiplies the
            // running fragment alpha by saturate(vertexColor.a). PlayCanvas can leave
            // alpha at zero because their decal example uses emissiveVertexColor (RGB
            // only); we route through diffuseVertexColor + UNLIT and need full alpha.
            v[OFF_COLOR + 3] = d.a;
        }
    }
}

int main()
{
    log::init();
    log::set_level_debug();

    const auto shutdown = [] {
        if (renderer) {
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }
        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
    };

    spdlog::info("*** VisuTwin Mesh-Decals Example ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Mesh Decals",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL Window creation failed\n";
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "SDL Renderer creation failed\n";
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        std::cerr << "Unable to get render Metal layer\n";
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) {
        std::cerr << "Unable to create graphics device\n";
        shutdown();
        return -1;
    }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setSkyboxMip(2);
    scene->setExposure(0.6f);
    scene->setAmbientLight(0.2f, 0.2f, 0.2f);

    if (const auto envResource = helipad->resource()) {
        scene->setEnvAtlas(std::get<Texture*>(*envResource));
    } else {
        spdlog::warn("Helipad env atlas missing — scene will be flat-lit");
    }

    Texture* heartTexture = nullptr;
    if (const auto heartResource = heart->resource()) {
        heartTexture = std::get<Texture*>(*heartResource);
    } else {
        spdlog::error("heart.png missing at {}/textures/heart.png", rootPath);
        shutdown();
        return -1;
    }

    // ── Ground plane ────────────────────────────────────────────────────
    auto* planeMaterial = new StandardMaterial();
    planeMaterial->setGloss(0.6f);
    planeMaterial->setMetalness(0.5f);
    planeMaterial->setUseMetalness(true);
    planeMaterial->setDiffuse(Color(0.55f, 0.55f, 0.6f, 1.0f));

    auto* plane = new Entity();
    plane->setEngine(engine.get());
    plane->setLocalScale(20.0f, 1.0f, 20.0f);
    if (auto* rc = static_cast<RenderComponent*>(plane->addComponent<RenderComponent>())) {
        rc->setMaterial(planeMaterial);
        rc->setType("plane");
    }
    engine->root()->addChild(plane);

    // ── Bouncing ball ───────────────────────────────────────────────────
    auto* ballMaterial = new StandardMaterial();
    ballMaterial->setDiffuse(Color(0.9f, 0.4f, 0.4f, 1.0f));
    ballMaterial->setGloss(0.4f);

    auto* ball = new Entity();
    ball->setEngine(engine.get());
    ball->setLocalScale(0.5f, 0.5f, 0.5f);
    if (auto* rc = static_cast<RenderComponent*>(ball->addComponent<RenderComponent>())) {
        rc->setMaterial(ballMaterial);
        rc->setType("sphere");
    }
    engine->root()->addChild(ball);

    // ── Omni light to make the ball pop a little ────────────────────────
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lc->setType(LightType::LIGHTTYPE_OMNI);
        lc->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lc->setIntensity(2.0f);
        lc->setRange(30.0f);
    }
    light->setLocalPosition(0.0f, 8.0f, 0.0f);
    engine->root()->addChild(light);

    // ── Decal mesh: dynamic vertex buffer + static index buffer ─────────
    // Build CPU-side storage; we re-upload positions/colors via setData() each frame
    // a decal is added or fades. Static indices never change after init.
    std::vector<float> decalVertices(static_cast<size_t>(TOTAL_VERTS) * FLOATS_PER_VERTEX, 0.0f);

    // Indices: two triangles per quad (0,1,2)(2,3,0).
    std::vector<uint16_t> decalIndices(TOTAL_INDICES);
    for (int i = 0; i < NUM_DECALS; ++i) {
        const uint16_t base = static_cast<uint16_t>(i * VERTS_PER_DECAL);
        decalIndices[i * 6 + 0] = base + 0;
        decalIndices[i * 6 + 1] = base + 1;
        decalIndices[i * 6 + 2] = base + 2;
        decalIndices[i * 6 + 3] = base + 2;
        decalIndices[i * 6 + 4] = base + 3;
        decalIndices[i * 6 + 5] = base + 0;
    }

    auto vertexFormat = std::make_shared<VertexFormat>(VERTEX_STRIDE_BYTES, true, false);

    VertexBufferOptions vbOpts;
    vbOpts.usage = BUFFER_DYNAMIC;
    vbOpts.data.resize(decalVertices.size() * sizeof(float));
    std::memcpy(vbOpts.data.data(), decalVertices.data(), vbOpts.data.size());
    auto decalVertexBuffer = graphicsDevice->createVertexBuffer(vertexFormat, TOTAL_VERTS, vbOpts);

    std::vector<uint8_t> indexBytes(decalIndices.size() * sizeof(uint16_t));
    std::memcpy(indexBytes.data(), decalIndices.data(), indexBytes.size());
    auto decalIndexBuffer = graphicsDevice->createIndexBuffer(INDEXFORMAT_UINT16, TOTAL_INDICES, indexBytes);

    auto decalMesh = std::make_shared<Mesh>();
    decalMesh->setVertexBuffer(decalVertexBuffer);
    decalMesh->setIndexBuffer(decalIndexBuffer);
    Primitive prim;
    prim.type = PRIMITIVE_TRIANGLES;
    prim.indexed = true;
    prim.base = 0;
    prim.count = TOTAL_INDICES;
    decalMesh->setPrimitive(prim);
    // Loose AABB covering the 20×20 plane area; decals stay within.
    decalMesh->setAabb(BoundingBox(Vector3(0, 0.05f, 0), Vector3(12, 0.5f, 12)));

    // ── Decal material ──────────────────────────────────────────────────
    // The PlayCanvas reference enables emissiveVertexColor + opacityMap to use the
    // emissive lane. visutwin-canvas doesn't yet have an emissiveVertexColor route,
    // so we use the diffuse lane via UNLIT (skips PBR lighting entirely) + the
    // texture's alpha for cutout. Result is visually equivalent: heart silhouettes
    // tinted by per-vertex color, accumulated additively.
    auto decalMaterial = std::make_shared<StandardMaterial>();
    decalMaterial->setUseLighting(false);                  // → VT_FEATURE_UNLIT
    decalMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
    decalMaterial->setDiffuseMap(heartTexture);            // alpha channel = cutout
    // Vertex colors modulate baseColor in the unlit path (forward-fragment-head.metal:179).
    decalMaterial->setShaderVariantKey(decalMaterial->shaderVariantKey() | (1ull << 21));
    decalMaterial->setTransparent(true);                   // route through transparent sublayer

    auto blend = std::make_shared<BlendState>(BlendState::additiveBlend());  // BLEND_ADDITIVEALPHA
    decalMaterial->setBlendState(blend);

    auto decalDepth = std::make_shared<DepthState>();
    decalDepth->setDepthTest(true);
    decalDepth->setDepthWrite(false);  // host plane already wrote depth; don't double-up
    // Polygon offset to keep decals visually on top of the plane. Negative bias pulls
    // fragments toward the camera in reverse-Z (matches Material.depthBias = -0.1 in upstream).
    decalDepth->setDepthBias(-0.1f);
    decalDepth->setSlopeDepthBias(-0.1f);
    decalMaterial->setDepthState(decalDepth);

    // Wrap the dynamic mesh in a MeshInstance hosted by an empty entity.
    auto* decalEntity = new Entity();
    decalEntity->setEngine(engine.get());
    auto decalNode = decalEntity;

    auto decalMeshInstance = std::make_unique<MeshInstance>(
        decalMesh.get(), decalMaterial.get(), decalNode);
    decalMeshInstance->setCastShadow(false);
    decalMeshInstance->setReceiveShadow(false);

    if (auto* rc = static_cast<RenderComponent*>(decalEntity->addComponent<RenderComponent>())) {
        rc->setMaterial(decalMaterial.get());
        rc->setCastShadows(false);
        rc->setReceiveShadows(false);
        rc->addMeshInstance(std::move(decalMeshInstance));
    }
    engine->root()->addChild(decalEntity);

    // ── Camera ──────────────────────────────────────────────────────────
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    if (auto* cc = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>())) {
        if (cc->camera()) {
            cc->camera()->setClearColor(Color(0.2f, 0.2f, 0.2f, 1.0f));
        }
    }
    camera->setPosition(20.0f, 10.0f, 20.0f);
    engine->root()->addChild(camera);

    // ── Per-frame state ─────────────────────────────────────────────────
    std::mt19937 rng(2026u);
    std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
    std::vector<DecalQuad> decals(NUM_DECALS);

    auto stampDecal = [&](int slot, const Vector3& at) {
        DecalQuad d;
        d.center = Vector3(at.getX(), 0.0f, at.getZ());
        d.size = 0.5f + rand01(rng);
        d.angleRad = rand01(rng) * 3.14159265f;
        d.r = rand01(rng);
        d.g = rand01(rng);
        d.b = rand01(rng);
        d.a = 1.0f;
        decals[slot] = d;
        writeDecal(decalVertices, slot, d);
    };

    auto uploadVertexBuffer = [&] {
        std::vector<uint8_t> bytes(decalVertices.size() * sizeof(float));
        std::memcpy(bytes.data(), decalVertices.data(), bytes.size());
        decalVertexBuffer->setData(bytes);
        decalVertexBuffer->unlock();
    };

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    float t = 0.0f;
    int decalSlot = 0;
    float fadeAccumulator = 0.0f;
    float cameraOrbit = 0.0f;

    spdlog::info("Mesh-Decals: bouncing ball stamps colored hearts on the ground plane. ESC to exit.");

    while (running) {
        SDL_Event evt;
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_EVENT_QUIT ||
                (evt.type == SDL_EVENT_KEY_DOWN && evt.key.key == SDLK_ESCAPE)) {
                running = false;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(
            static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq));
        prevCounter = nowCounter;

        const float prevT = t;
        t += dt;

        // Bouncing ball: orbit with varying radius, sin-vertical bounce.
        const float radius = std::abs(std::sin(t * 0.55f) * 9.0f);
        const float prevElev = 2.0f * std::cos(prevT * 7.0f);
        const float elev = 2.0f * std::cos(t * 7.0f);
        ball->setLocalPosition(
            radius * std::sin(t),
            0.5f + std::abs(elev),
            radius * std::cos(t));

        // Stamp a decal each time the ball crosses y=0 (sign change of elev).
        bool dirty = false;
        if ((prevElev < 0.0f && elev >= 0.0f) || (elev < 0.0f && prevElev >= 0.0f)) {
            stampDecal(decalSlot, ball->localPosition());
            decalSlot = (decalSlot + 1) % NUM_DECALS;
            dirty = true;
        }

        // Fade all existing decals once per second by reducing vertex color magnitude.
        // upstream fades vertex color bytes by 2 each second (out of 255). We mimic that
        // ratio with a normalized 2/255 ≈ 0.0078 step on float colors.
        fadeAccumulator += dt;
        if (fadeAccumulator >= 1.0f) {
            fadeAccumulator -= 1.0f;
            constexpr float kFadeStep = 2.0f / 255.0f;
            for (int i = 0; i < NUM_DECALS; ++i) {
                auto& d = decals[i];
                d.r = std::max(0.0f, d.r - kFadeStep);
                d.g = std::max(0.0f, d.g - kFadeStep);
                d.b = std::max(0.0f, d.b - kFadeStep);
                writeDecal(decalVertices, i, d);
            }
            dirty = true;
        }

        if (dirty) {
            uploadVertexBuffer();
        }

        // Orbit camera around origin.
        cameraOrbit += dt * 0.3f;
        camera->setLocalPosition(
            20.0f * std::sin(cameraOrbit),
            10.0f,
            20.0f * std::cos(cameraOrbit));
        const float yawDeg =
            std::atan2(20.0f * std::sin(cameraOrbit), 20.0f * std::cos(cameraOrbit)) *
            (180.0f / 3.14159265f);
        camera->setLocalEulerAngles(-25.0f, yawDeg, 0.0f);

        engine->update(dt);
        engine->render();
    }

    shutdown();
    spdlog::info("*** VisuTwin Mesh-Decals Example Finished ***");
    return 0;
}
