// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Demonstrates GPU-driven instance culling on top of hardware instancing.
// 10,000 randomly placed cylinders are spread across a large volume; each
// frame, a Metal compute pass (MetalInstanceCullPass) tests each instance's
// bounding sphere against the camera frustum and produces a compacted instance
// buffer + indirect draw arguments. The draw call is issued via indirect
// instancing so only visible instances are rendered.
//
// Compared to instancing-basic-example: same mesh, same material, same
// setInstancing() API — the only new call is enableGpuInstanceCulling(device, r).
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <cmath>
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
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/instanceCuller.h"
#include "platform/graphics/vertexFormat.h"
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

    spdlog::info("*** VisuTwin Instancing-Culled Example ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Instancing Culled",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        std::cerr << "SDL Window Creation Failed" << std::endl;
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::cerr << "SDL Renderer Creation Failed" << std::endl;
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        std::cerr << "Unable to get render Metal layer" << std::endl;
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(
        GraphicsDeviceOptions{.swapChain = swapchain, .window = window}
    );
    if (!device) {
        std::cerr << "Unable to create graphics device" << std::endl;
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

    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

    engine->start();

    auto scene = engine->scene();
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

    // Camera — pulled back further to see the wide volume.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    engine->root()->addChild(camera);
    camera->setPosition(0.0f, 0.0f, 60.0f);

    auto material = std::make_shared<StandardMaterial>();
    material->setGloss(0.6f);
    material->setMetalness(0.7f);
    material->setUseMetalness(true);
    // Enable instancing shader variant (bit 33).
    material->setShaderVariantKey(material->shaderVariantKey() | (1ull << 33));

    auto* cylinder = new Entity();
    cylinder->setName("InstancingCulledEntity");
    cylinder->setEngine(engine.get());
    auto* renderComp = static_cast<RenderComponent*>(cylinder->addComponent<RenderComponent>());
    if (renderComp) {
        renderComp->setMaterial(material.get());
        renderComp->setType("cylinder");
    }
    engine->root()->addChild(cylinder);

    // ── 10,000 instances spread over a 60×60×60 volume ────────────
    constexpr int instanceCount = 10000;
    constexpr int kInstanceDataBytes = 80;
    constexpr float kSpreadRadius = 30.0f;  // volume half-extent

    std::vector<uint8_t> instanceBytes(
        static_cast<size_t>(instanceCount) * kInstanceDataBytes);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> distPos(-kSpreadRadius, kSpreadRadius);
    std::uniform_real_distribution<float> distCol(0.3f, 1.0f);

    for (int i = 0; i < instanceCount; ++i) {
        const Vector3 pos(distPos(rng), distPos(rng), distPos(rng));

        // Uniform-ish scale so the bounding sphere radius is predictable.
        const Vector3 scl(0.2f, 0.4f, 0.2f);

        const auto rot = Quaternion::fromEulerAngles(
            static_cast<float>(i) * 30.0f,
            static_cast<float>(i) * 50.0f,
            static_cast<float>(i) * 70.0f
        );

        const auto matrix = Matrix4::trs(pos, rot, scl);

        auto* dst = instanceBytes.data() + static_cast<size_t>(i) * kInstanceDataBytes;
        std::memcpy(dst, &matrix, 64);

        // Random-ish color to make culling visible.
        const float color[4] = { distCol(rng), distCol(rng), distCol(rng), 1.0f };
        std::memcpy(dst + 64, color, 16);
    }

    auto instanceFormat = std::make_shared<VertexFormat>(kInstanceDataBytes, false, true);
    VertexBufferOptions vbOptions;
    vbOptions.data = std::move(instanceBytes);
    auto instanceBuffer = graphicsDevice->createVertexBuffer(
        instanceFormat, instanceCount, vbOptions);

    MeshInstance* cylinderMeshInst = nullptr;
    if (renderComp && !renderComp->meshInstances().empty()) {
        cylinderMeshInst = renderComp->meshInstances()[0];
        if (cylinderMeshInst) {
            cylinderMeshInst->setInstancing(instanceBuffer, instanceCount);

            // ── Enable GPU frustum culling ────────────────────────
            //
            // The largest scale axis is 0.4 and the cylinder primitive
            // has a unit-radius, unit-height base, so a bounding sphere
            // of radius ~0.6 comfortably covers each instance with a
            // small safety margin.
            cylinderMeshInst->enableGpuInstanceCulling(
                graphicsDevice.get(), /*boundingSphereRadius=*/0.6f);

            if (cylinderMeshInst->gpuCullingEnabled()) {
                spdlog::info("Example: GPU-culled instancing active ({} source instances)",
                    instanceCount);
            } else {
                spdlog::error("Example: enableGpuInstanceCulling failed — culling disabled");
            }
        }
    } else {
        spdlog::warn("No mesh instances found on cylinder render component");
    }

    // ── Main loop ─────────────────────────────────────────────────
    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float angle = 0.0f;
    double secondsAccum = 0.0;
    int frameCountAccum = 0;

    spdlog::info("Instancing-Culled: {} cylinders, GPU frustum culling. ESC to exit.",
        instanceCount);

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
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) /
                                 static_cast<double>(perfFreq);
        prevCounter = nowCounter;
        const float dt = static_cast<float>(dtSeconds);

        // Orbit camera at a moderate radius so most instances are out of frustum.
        angle += dt * 0.3f;
        const float camDist = 50.0f;
        const float cx = camDist * std::sin(angle);
        const float cz = camDist * std::cos(angle);
        camera->setLocalPosition(cx, 0.0f, cz);

        // Look at origin.
        const float yaw = std::atan2(cx, cz) * (180.0f / 3.14159265358979323846f);
        camera->setLocalEulerAngles(0.0f, yaw, 0.0f);

        engine->update(dt);
        engine->render();

        // Once per second, print visible instance count + FPS to confirm
        // the cull kernel is producing sensible numbers as the camera orbits.
        secondsAccum += dtSeconds;
        frameCountAccum += 1;
        if (secondsAccum >= 1.0) {
            const double fps = static_cast<double>(frameCountAccum) / secondsAccum;
            uint32_t visible = 0;
            if (cylinderMeshInst && cylinderMeshInst->instanceCuller()) {
                visible = cylinderMeshInst->instanceCuller()->visibleCountReadback();
            }
            spdlog::info("Frame stats: {:.1f} FPS | visible {}/{} instances",
                fps, visible, instanceCount);
            secondsAccum = 0.0;
            frameCountAccum = 0;
        }
    }

    shutdown();

    spdlog::info("*** VisuTwin Instancing-Culled Example Finished ***");

    return 0;
}
