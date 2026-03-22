// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Texture Streaming Example — demonstrates MetalTextureStream with a
// scrolling gradient pattern rendered onto a plane.
//
// The producer writes a synthetic scrolling gradient into the triple-buffered
// stream each frame. The consumer acquires the latest texture and injects it
// into the material system via MetalTexture::setExternalTexture().
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#ifdef VISUTWIN_HAS_METAL
#include "platform/graphics/metal/metalGraphicsDevice.h"
#include "platform/graphics/metal/metalTexture.h"
#include "platform/graphics/metal/metalTextureStream.h"
#endif
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH  = 900;
constexpr int WINDOW_HEIGHT = 700;

// Stream texture resolution
constexpr uint32_t STREAM_WIDTH  = 512;
constexpr uint32_t STREAM_HEIGHT = 512;

using namespace visutwin::canvas;

SDL_Window*   window   = nullptr;
SDL_Renderer* renderer = nullptr;

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

/// Generate a scrolling BGRA gradient pattern.
/// Each pixel encodes a rainbow-ish color that scrolls vertically with time.
void generateScrollingGradient(std::vector<uint8_t>& pixels,
                                uint32_t width, uint32_t height,
                                float time)
{
    const float scrollSpeed = 80.0f;  // pixels per second
    const float offset = std::fmod(time * scrollSpeed, static_cast<float>(height));

    for (uint32_t y = 0; y < height; ++y) {
        const float ty = std::fmod(static_cast<float>(y) + offset, static_cast<float>(height))
                         / static_cast<float>(height);

        for (uint32_t x = 0; x < width; ++x) {
            const float tx = static_cast<float>(x) / static_cast<float>(width);

            // HSV-like rainbow: hue varies with y, saturation with x
            const float hue = ty * 6.0f;
            const int hi = static_cast<int>(hue) % 6;
            const float f = hue - std::floor(hue);
            const float q = 1.0f - f;

            float r = 0.0f, g = 0.0f, b = 0.0f;
            switch (hi) {
                case 0: r = 1.0f; g = f;    b = 0.0f; break;
                case 1: r = q;    g = 1.0f; b = 0.0f; break;
                case 2: r = 0.0f; g = 1.0f; b = f;    break;
                case 3: r = 0.0f; g = q;    b = 1.0f; break;
                case 4: r = f;    g = 0.0f; b = 1.0f; break;
                case 5: r = 1.0f; g = 0.0f; b = q;    break;
            }

            // Desaturate towards center (based on x)
            const float sat = 0.5f + 0.5f * std::sin(tx * 3.14159f * 2.0f);
            r = r * sat + (1.0f - sat) * 0.5f;
            g = g * sat + (1.0f - sat) * 0.5f;
            b = b * sat + (1.0f - sat) * 0.5f;

            const size_t idx = (y * width + x) * 4;
            pixels[idx + 0] = static_cast<uint8_t>(b * 255.0f);  // B
            pixels[idx + 1] = static_cast<uint8_t>(g * 255.0f);  // G
            pixels[idx + 2] = static_cast<uint8_t>(r * 255.0f);  // R
            pixels[idx + 3] = 255;                                 // A
        }
    }
}

int main()
{
    log::init();
    log::set_level_debug();

    const auto shutdown = []() {
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window)   { SDL_DestroyWindow(window);     window   = nullptr; }
        SDL_Quit();
    };

    spdlog::info("*** Texture Stream Example ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Texture Streaming",
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) { shutdown(); return -1; }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { shutdown(); return -1; }

    auto device = createGraphicsDevice(
        GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) { shutdown(); return -1; }

#ifdef VISUTWIN_HAS_METAL
    auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(device.get());
    if (!metalDevice) {
        spdlog::error("Texture streaming requires a Metal graphics device");
        shutdown();
        return -1;
    }
#else
    spdlog::error("Texture streaming example requires Metal backend");
    shutdown();
    return -1;
#endif

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
    scene->setAmbientLight(0.5f, 0.5f, 0.5f);
    scene->setSkyboxMip(1);

    const auto helipadResource = helipad->resource();
    if (helipadResource) {
        scene->setEnvAtlas(std::get<Texture*>(*helipadResource));
    }

#ifdef VISUTWIN_HAS_METAL
    // ── Create the triple-buffered texture stream ─────────────────────
    MetalTextureStream::Descriptor streamDesc;
    streamDesc.width  = STREAM_WIDTH;
    streamDesc.height = STREAM_HEIGHT;
    streamDesc.format = MTL::PixelFormatBGRA8Unorm;
    streamDesc.label  = "GradientStream";

    MetalTextureStream stream(metalDevice->raw(), streamDesc);
#endif

    // ── Create an engine Texture as a placeholder for the stream ──────
    // The material system needs an engine Texture; we'll swap its underlying
    // MTL::Texture each frame via setExternalTexture().
    TextureOptions texOpts;
    texOpts.width   = STREAM_WIDTH;
    texOpts.height  = STREAM_HEIGHT;
    texOpts.format  = PixelFormat::PIXELFORMAT_RGBA8;
    texOpts.mipmaps = false;
    texOpts.name    = "streamPlaceholder";
    auto streamTexture = std::make_shared<Texture>(graphicsDevice.get(), texOpts);
    // Force GPU-side texture creation so impl() returns a valid MetalTexture
    streamTexture->upload();

    // ── Create material with the stream texture as diffuse map ────────
    auto streamMaterial = std::make_shared<StandardMaterial>();
    streamMaterial->setName("streamMaterial");
    streamMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
    streamMaterial->setDiffuseMap(streamTexture.get());

    // ── Create a plane to display the streamed texture ────────────────
    auto* planeEntity = new Entity();
    planeEntity->setEngine(engine.get());
    planeEntity->setLocalPosition(0.0f, 0.0f, 0.0f);
    planeEntity->setLocalScale(4.0f, 1.0f, 4.0f);
    planeEntity->setLocalEulerAngles(0.0f, 0.0f, 0.0f);

    auto* renderComp = static_cast<RenderComponent*>(
        planeEntity->addComponent<RenderComponent>());
    if (renderComp) {
        renderComp->setMaterial(streamMaterial.get());
        renderComp->setType("plane");
    }
    engine->root()->addChild(planeEntity);

    // ── Create a light ────────────────────────────────────────────────
    auto* lightEntity = new Entity();
    lightEntity->setEngine(engine.get());
    lightEntity->addComponent<LightComponent>();
    lightEntity->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(lightEntity);

    // ── Create a camera looking down at the plane ─────────────────────
    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    cameraEntity->addComponent<CameraComponent>();
    cameraEntity->setPosition(0.0f, 5.0f, 5.0f);
    cameraEntity->setLocalEulerAngles(-40.0f, 0.0f, 0.0f);
    engine->root()->addChild(cameraEntity);

    // ── Pixel buffer for CPU-side gradient generation ─────────────────
    std::vector<uint8_t> pixelData(STREAM_WIDTH * STREAM_HEIGHT * 4);

    // ── Main loop ─────────────────────────────────────────────────────
    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float totalTime = 0.0f;
    uint64_t frameCount = 0;
    float logTimer = 0.0f;

    spdlog::info("Controls: ESC to quit");

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
        const float dt = static_cast<float>(
            static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq));
        prevCounter = nowCounter;
        totalTime += dt;
        frameCount++;
        logTimer += dt;

#ifdef VISUTWIN_HAS_METAL
        // ── Producer: generate and upload gradient to the stream ──────
        generateScrollingGradient(pixelData, STREAM_WIDTH, STREAM_HEIGHT, totalTime);

        stream.beginWrite();
        stream.writeRegion(
            pixelData.data(),
            STREAM_WIDTH * 4,  // bytesPerRow
            MTL::Region(0, 0, 0, STREAM_WIDTH, STREAM_HEIGHT, 1));
        stream.endWrite();

        // ── Consumer: inject the latest texture into the material ─────
        auto* readTex = stream.acquireForRead();
        if (readTex) {
            auto* metalTex = dynamic_cast<gpu::MetalTexture*>(streamTexture->impl());
            if (metalTex) {
                metalTex->setExternalTexture(readTex);
            }
        }
#endif

        // ── Engine update + render ────────────────────────────────────
        engine->update(dt);
        engine->render();

#ifdef VISUTWIN_HAS_METAL
        // ── Register GPU completion for the stream ────────────────────
        auto* cmdBuf = metalDevice->commandQueue()->commandBuffer();
        if (cmdBuf) {
            stream.endFrame(cmdBuf);
            cmdBuf->commit();
        }

        // ── Periodic stats logging ────────────────────────────────────
        if (logTimer >= 5.0f) {
            const float fps = static_cast<float>(frameCount) / totalTime;
            spdlog::info("[TextureStream] fps={:.1f} | published={} dropped={} | {:.1f}s elapsed",
                fps, stream.framesPublished(), stream.framesDropped(), totalTime);
            logTimer = 0.0f;
        }
#endif
    }

#ifdef VISUTWIN_HAS_METAL
    // ── Cleanup ───────────────────────────────────────────────────────
    // Clear the external texture reference before the stream is destroyed,
    // so the MetalTexture doesn't hold a dangling pointer.
    {
        auto* metalTex = dynamic_cast<gpu::MetalTexture*>(streamTexture->impl());
        if (metalTex) {
            metalTex->setExternalTexture(nullptr);
        }
    }
#endif

    shutdown();

    spdlog::info("*** Texture Stream Example Finished ***");
    return 0;
}
