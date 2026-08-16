// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream's graphics/instancing-basic: hardware instancing with a
// StandardMaterial — 1000 randomly placed, randomly scaled cylinders drawn in a
// single call from per-instance model matrices, lit by the helipad env atlas
// alone. The scene matches upstream value for value.
//
// The only deliberate difference is the fixed RNG seed, so the instance layout is
// reproducible for screenshot comparison; upstream reseeds from Math.random().
//
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#endif

#include <SDL3/SDL.h>
#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

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

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

int main()
{
    log::init();
    log::set_level_debug();

    const auto shutdown = []() {
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

    spdlog::info("*** Instancing-Basic Example ***");

#ifdef VISUTWIN_HAS_METAL
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Instancing Basic",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
#ifdef VISUTWIN_HAS_VULKAN
        | SDL_WINDOW_VULKAN
#endif
    );
    if (!window) {
        spdlog::error("SDL Window Creation Failed");
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        spdlog::error("SDL Renderer Creation Failed");
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    void* swapchain = nullptr;
#ifdef VISUTWIN_HAS_METAL
    swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        spdlog::error("Unable to get render Metal layer");
        shutdown();
        return -1;
    }
#endif

    GraphicsDeviceOptions deviceOptions;
#ifdef VISUTWIN_HAS_VULKAN
    deviceOptions.backend = Backend::Vulkan;
#endif
    deviceOptions.swapChain = swapchain;
    deviceOptions.window = window;
    auto device = createGraphicsDevice(deviceOptions);
    if (!device) {
        spdlog::error("Unable to create graphics device");
        shutdown();
        return -1;
    }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);

    // Set the canvas to fill the window and automatically change resolution to be the same as the canvas size
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

    engine->start();

    auto scene = engine->scene();

    // setup skydome — JS: app.scene.skyboxMip = 2; app.scene.exposure = 0.3;
    scene->setSkyboxMip(2);
    scene->setExposure(0.3f);
    scene->setAmbientLight(0.1f, 0.1f, 0.1f);

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    // Create an Entity with a camera component
    // JS: camera.addComponent('camera', { toneMapping: TONEMAP_ACES })
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    if (cameraComp) {
        cameraComp->setToneMapping(TONEMAP_ACES);
    }
    engine->root()->addChild(camera);

    // Move the camera back to see the cylinders
    camera->setPosition(0.0f, 0.0f, 10.0f);

    // Create standard material — instancing needs nothing from the material, the
    // instanced shader variant follows the mesh instance's per-instance buffer.
    // JS: material.gloss = 0.6; material.metalness = 0.7; material.useMetalness = true;
    auto material = std::make_shared<StandardMaterial>();
    material->setGloss(0.6f);
    material->setMetalness(0.7f);
    material->setUseMetalness(true);

    // Create a Entity with a cylinder render component and the instancing material
    auto* cylinder = new Entity();
    cylinder->setName("InstancingEntity");
    cylinder->setEngine(engine.get());
    auto* renderComp = static_cast<RenderComponent*>(cylinder->addComponent<RenderComponent>());
    if (renderComp) {
        renderComp->setMaterial(material.get());
        renderComp->setType("cylinder");
    }
    engine->root()->addChild(cylinder);

    // Number of instances to render
    constexpr int instanceCount = 1000;

    // ── Build per-instance data buffer ─────────────────────────────
    //
    // JS: const matrices = new Float32Array(instanceCount * 16)
    // One column-major float4x4 model matrix per instance; base color comes from the
    // material, as in any other draw (VertexFormat::defaultInstancingFormat below).
    constexpr int kInstanceDataBytes = VertexFormat::INSTANCING_MATRIX_SIZE;

    std::vector<uint8_t> instanceBytes(static_cast<size_t>(instanceCount) * kInstanceDataBytes);

    std::mt19937 rng(42);  // fixed seed for deterministic layout
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);

    constexpr float radius = 5.0f;

    for (int i = 0; i < instanceCount; ++i) {
        // Generate random positions, scales and rotations — matching JS:
        // pos.set(random * radius - radius * 0.5, ...)
        const float px = dist01(rng) * radius - radius * 0.5f;
        const float py = dist01(rng) * radius - radius * 0.5f;
        const float pz = dist01(rng) * radius - radius * 0.5f;

        // JS: scl.set(0.1 + random * 0.1, 0.1 + random * 0.3, 0.1 + random * 0.1)
        const float sx = 0.1f + dist01(rng) * 0.1f;
        const float sy = 0.1f + dist01(rng) * 0.3f;
        const float sz = 0.1f + dist01(rng) * 0.1f;

        // JS: rot.setFromEulerAngles(i * 30, i * 50, i * 70)
        const auto rot = Quaternion::fromEulerAngles(
            static_cast<float>(i) * 30.0f,
            static_cast<float>(i) * 50.0f,
            static_cast<float>(i) * 70.0f
        );

        const auto pos = Vector3(px, py, pz);
        const auto scl = Vector3(sx, sy, sz);

        // Build TRS matrix
        const auto matrix = Matrix4::trs(pos, rot, scl);

        // Copy matrix elements into the instance buffer (JS copies matrix.data)
        auto* dst = instanceBytes.data() + static_cast<size_t>(i) * kInstanceDataBytes;
        std::memcpy(dst, &matrix, kInstanceDataBytes);
    }

    // Create static vertex buffer containing the matrices
    // JS: VertexFormat.getDefaultInstancingFormat(app.graphicsDevice)
    auto instanceFormat = VertexFormat::defaultInstancingFormat();
    VertexBufferOptions vbOptions;
    vbOptions.data = std::move(instanceBytes);
    auto instanceBuffer = graphicsDevice->createVertexBuffer(instanceFormat, instanceCount, vbOptions);

    // Initialize instancing using the vertex buffer on meshInstance of the created cylinder
    if (renderComp && !renderComp->meshInstances().empty()) {
        auto* cylinderMeshInst = renderComp->meshInstances()[0];
        if (cylinderMeshInst) {
            cylinderMeshInst->setInstancing(instanceBuffer, instanceCount);
            spdlog::info("Instancing enabled: {} instances", instanceCount);
        }
    } else {
        spdlog::warn("No mesh instances found on cylinder render component");
    }

    // ── Main loop ──────────────────────────────────────────────────
    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float angle = 0.0f;

    spdlog::info("Instancing-Basic: {} cylinders in one draw call. ESC to exit.", instanceCount);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        const float dt = static_cast<float>(dtSeconds);

        // Orbit camera around — JS: angle += dt * 0.2;
        angle += dt * 0.2f;
        camera->setLocalPosition(
            8.0f * std::sin(angle),
            0.0f,
            8.0f * std::cos(angle)
        );

        // JS: camera.lookAt(Vec3.ZERO)
        camera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        engine->update(dt);
        engine->render();
    }

    shutdown();

    spdlog::info("*** Instancing-Basic Example Finished ***");

    return 0;
}
