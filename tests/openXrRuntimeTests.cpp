// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 20.11.2026
//
// A session through a real OpenXR runtime, on this build's backend: the graphics device is
// created as the runtime asks, the session is started, and frames are rendered into the
// runtime's own swapchain images until several have been drawn and submitted. A box held
// straight ahead of the head must then be in both eyes' images, right of centre in the left
// eye and left of centre in the right.
//
// With no runtime, or no headset, the test says so and passes: it proves something only
// where a runtime is active (Monado with its simulated headset serves; XR_RUNTIME_JSON picks
// one). The images are read back after their frame was handed to the runtime, which a test
// may get away with and an application may not.
//
// It needs a GPU and a window, so it carries the `gpu` label.
//
// The metal-cpp *_PRIVATE_IMPLEMENTATION translation unit of this executable: a program
// linking the static engine supplies it (see examples/exampleApp.cpp).
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/xr/openxr/openXrBackend.h"
#include "framework/xr/xrBackend.h"
#include "framework/xr/xrManager.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"
#include "support/check.h"
#include "support/testEngine.h"

#ifdef VISUTWIN_HAS_VULKAN
#include <atomic>

#include "platform/graphics/vulkan/vulkanGraphicsDevice.h"
#endif

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    struct Blob
    {
        int count = 0;
        double x = 0.0;
        double y = 0.0;
        int width = 0;
        int height = 0;
    };

    /// The centroid of the red pixels of a view's image, whichever way its bytes are ordered.
    Blob findRed(Texture* texture)
    {
        Blob blob;
        std::vector<uint8_t> pixels;
        if (!texture || !texture->read(pixels)) {
            return blob;
        }
        blob.width = static_cast<int>(texture->width());
        blob.height = static_cast<int>(texture->height());
        const int red = texture->format() == PixelFormat::PIXELFORMAT_BGRA8 ? 2 : 0;
        const int blue = 2 - red;
        if (pixels.size() < static_cast<size_t>(blob.width) * blob.height * 4) {
            return blob;
        }
        for (int y = 0; y < blob.height; ++y) {
            for (int x = 0; x < blob.width; ++x) {
                const uint8_t* p = &pixels[(static_cast<size_t>(y) * blob.width + x) * 4];
                if (p[red] > 160 && p[1] < 90 && p[blue] < 90) {
                    ++blob.count;
                    blob.x += x + 0.5;
                    blob.y += y + 0.5;
                }
            }
        }
        if (blob.count > 0) {
            blob.x /= blob.count;
            blob.y /= blob.count;
        }
        return blob;
    }
}

int main()
{
    const auto backendXr = createOpenXrBackend();
    if (!backendXr) {
        std::cout << "openxr runtime: skipped, the engine is built without OpenXR\n";
        return 0;
    }
    if (!backendXr->isAvailable(XrSessionType::Vr)) {
        std::cout << "openxr runtime: skipped, no OpenXR runtime with a headset is active\n";
        return 0;
    }

    const Backend backend = defaultBackend();
    if (backend == Backend::Metal) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }
    const SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | (backend == Backend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL);
    SDL_Window* window = SDL_CreateWindow("openxr", 256, 256, flags);
    SDL_Renderer* renderer = backend == Backend::Metal ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!window || (backend == Backend::Metal && !renderer)) {
        std::cerr << "window or renderer failed: " << SDL_GetError() << '\n';
        return 1;
    }

    {
        GraphicsDeviceOptions deviceOptions;
        deviceOptions.backend = backend;
        deviceOptions.window = window;
#ifndef NDEBUG
        deviceOptions.enableValidation = true;
#endif
        if (renderer) {
            deviceOptions.swapChain = SDL_GetRenderMetalLayer(renderer);
        }
        // The runtime's say in the device: its extensions and its GPU.
        backendXr->configureGraphicsDevice(deviceOptions);
        std::shared_ptr<GraphicsDevice> device = createGraphicsDevice(deviceOptions);
        check(device != nullptr, std::string("a ") + backendName(backend) + " device is created as the runtime asks");
        if (!device) {
            return 1;
        }

        const auto xr = std::make_shared<XrManager>(backendXr);
        auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device,
            [&](AppOptions& options) { options.xr = xr; });

        const auto red = std::make_shared<StandardMaterial>();
        red->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        red->setEmissive(Color(1.0f, 0.0f, 0.0f, 1.0f));
        red->setUnlit(true);
        auto* box = addTo(engine->root(), newEntity(engine.get(), "box"));
        box->setLocalScale(0.3f, 0.3f, 0.3f);
        auto* render = static_cast<RenderComponent*>(box->addComponent<RenderComponent>());
        render->setType("box");
        render->setMaterial(red.get());

        auto* rig = addTo(engine->root(), newEntity(engine.get(), "rig"));
        auto* cameraEntity = addTo(rig, newEntity(engine.get(), "camera"));
        auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
        cameraComponent->camera()->setClearColor(Color(0.0f, 0.0f, 1.0f, 1.0f));

        engine->start();

        std::string reason;
        XrSessionOptions sessionOptions;
        sessionOptions.spaceType = XrSpaceType::Local;
        const bool started = xr->start(cameraComponent, sessionOptions,
            [&](const std::string* error) { reason = error ? *error : ""; });
        check(started, "a session starts on the runtime" + (reason.empty() ? std::string() : ": " + reason));
        if (started) {
            const auto size = backendXr->viewSize();
            std::cout << "  runtime views: " << size.first << " x " << size.second << '\n';

            // The runtime readies the session in its own time; then it paces the frames.
            int rendered = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            for (int frame = 0; frame < 2000 && rendered < 5 && xr->active() &&
                    std::chrono::steady_clock::now() < deadline; ++frame) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                }
                engine->waitForNextFrame();
                engine->update(1.0f / 60.0f);
                // Held 2 m ahead of the head, wherever the runtime puts it.
                if (!xr->views().empty()) {
                    const Vector3 ahead = cameraEntity->position() + cameraEntity->rotation() * Vector3(0.0f, 0.0f, -2.0f);
                    box->setPosition(ahead.getX(), ahead.getY(), ahead.getZ());
                    box->setRotation(cameraEntity->rotation());
                }
                engine->render();
                if (!cameraComponent->camera()->views().empty()) {
                    ++rendered;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            check(xr->active(), "the session is still running");
            check(rendered >= 5, "frames are rendered into the runtime's images: " + std::to_string(rendered));
            check(xr->views().size() == 2 && xr->view(XrEye::Left) && xr->view(XrEye::Right),
                "the runtime gives two views: " + std::to_string(xr->views().size()));

            if (rendered >= 5 && xr->views().size() == 2) {
                const float ipd = (xr->views()[1].pose.position - xr->views()[0].pose.position).length();
                std::cout << "  eye separation: " << ipd << " m\n";
                const Blob left = findRed(backendXr->viewTexture(0, 0));
                const Blob right = findRed(backendXr->viewTexture(1, 0));
                check(left.count > 20 && right.count > 20, "both eyes' images hold the box: " +
                    std::to_string(left.count) + ", " + std::to_string(right.count) + " pixels");
                if (left.count > 0 && right.count > 0 && ipd > 0.001f) {
                    check(left.x > right.x + 1.0, "nearer the right edge in the left eye than in the right: x = " +
                        std::to_string(left.x) + ", " + std::to_string(right.x) + " of " + std::to_string(left.width));
                }
            }
            xr->end();
            check(!xr->active(), "the session ends");
            engine->update(1.0f / 60.0f);
            engine->render();
        }
#ifdef VISUTWIN_HAS_VULKAN
        // Where the validation layer watched (a Debug device with the layer installed), the
        // whole session, the runtime's images included, raised nothing.
        if (auto* vulkan = dynamic_cast<VulkanGraphicsDevice*>(device.get())) {
            const auto errors = vulkan->validationErrorCounter();
            check(!errors || errors->load() == 0, "the Vulkan validation layer raised no error: " +
                std::to_string(errors ? errors->load() : 0));
        }
#endif
        xr->setBackend(nullptr);
        engine->destroy();
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("openxr runtime");
}
