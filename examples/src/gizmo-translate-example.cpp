// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/gizmo/transformGizmo.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 760;

using namespace visutwin::canvas;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

void setLookAt(Entity* entity, const Vector3& target)
{
    if (!entity) {
        return;
    }

    const Vector3 position = entity->position();
    const Vector3 lookDir = (target - position).normalized();
    const float pitchDeg = std::asin(std::clamp(lookDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yawDeg = std::atan2(-lookDir.getX(), -lookDir.getZ()) * RAD_TO_DEG;
    entity->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
}

Entity* createPrimitiveEntity(
    Engine* engine,
    const std::string& type,
    const Vector3& position,
    const Vector3& scale,
    StandardMaterial* material,
    const std::vector<int>& layers = {LAYERID_WORLD})
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position);
    entity->setLocalScale(scale);

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setType(type);
        render->setMaterial(material);
        render->setLayers(layers);
    }

    engine->root()->addChild(entity);
    return entity;
}

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

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Transform Gizmo (Translate/Rotate/Scale)",
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) {
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
    scene->setAmbientLight(0.25f, 0.25f, 0.25f);

    auto gridMaterial = std::make_shared<StandardMaterial>();
    gridMaterial->setDiffuse(Color(0.2f, 0.22f, 0.25f, 1.0f));

    auto boxMaterial = std::make_shared<StandardMaterial>();
    boxMaterial->setDiffuse(Color(0.82f, 0.48f, 0.16f, 1.0f));

    auto* grid = createPrimitiveEntity(
        engine.get(),
        "box",
        Vector3(0.0f, -0.05f, 0.0f),
        Vector3(8.0f, 0.1f, 8.0f),
        gridMaterial.get()
    );
    (void)grid;

    auto* box = createPrimitiveEntity(
        engine.get(),
        "box",
        Vector3(0.0f, 0.5f, 0.0f),
        Vector3(1.0f, 1.0f, 1.0f),
        boxMaterial.get()
    );

    auto* lightEntity = new Entity();
    lightEntity->setEngine(engine.get());
    auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    if (light) {
        light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        light->setIntensity(2.4f);
    }
    lightEntity->setLocalEulerAngles(45.0f, 35.0f, 0.0f);
    engine->root()->addChild(lightEntity);

    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    if (!camera || !camera->camera()) {
        spdlog::error("Failed to create camera");
        shutdown();
        return -1;
    }
    camera->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
    cameraEntity->setLocalPosition(4.2f, 4.2f, 4.2f);
    setLookAt(cameraEntity, Vector3(0.0f, 0.5f, 0.0f));
    engine->root()->addChild(cameraEntity);

    TransformGizmo gizmo(engine.get(), camera);
    gizmo.attach(box);
    gizmo.setMode(TransformGizmo::Mode::Translate);
    gizmo.setSnap(false);

    spdlog::info("Controls: 1=Translate, 2=Rotate, 3=Scale, S=Toggle Snap, [ / ] adjust snap increment");

    bool running = true;

    float orbitYaw = 45.0f;
    float orbitPitch = 25.0f;
    float orbitDist = 6.0f;
    bool orbiting = false;
    float prevMouseX = 0.0f;
    float prevMouseY = 0.0f;

    const Vector3 focusPoint(0.0f, 0.5f, 0.0f);

    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    float snapIncrement = 0.5f;

    while (running) {
        int winW = 0;
        int winH = 0;
        SDL_GetWindowSize(window, &winW, &winH);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
                continue;
            }

            if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    running = false;
                    continue;
                }
                if (event.key.key == SDLK_1) {
                    gizmo.setMode(TransformGizmo::Mode::Translate);
                    spdlog::info("Gizmo mode: Translate");
                } else if (event.key.key == SDLK_2) {
                    gizmo.setMode(TransformGizmo::Mode::Rotate);
                    spdlog::info("Gizmo mode: Rotate");
                } else if (event.key.key == SDLK_3) {
                    gizmo.setMode(TransformGizmo::Mode::Scale);
                    spdlog::info("Gizmo mode: Scale");
                } else if (event.key.key == SDLK_S) {
                    gizmo.setSnap(!gizmo.snap());
                    spdlog::info("Snap: {}", gizmo.snap() ? "ON" : "OFF");
                } else if (event.key.key == SDLK_LEFTBRACKET) {
                    snapIncrement = std::max(0.05f, snapIncrement - 0.05f);
                    gizmo.setTranslateSnapIncrement(snapIncrement);
                    gizmo.setScaleSnapIncrement(std::max(0.01f, snapIncrement * 0.2f));
                    gizmo.setRotateSnapIncrement(std::max(1.0f, snapIncrement * 20.0f));
                    spdlog::info("Snap increment: {:.2f}", snapIncrement);
                } else if (event.key.key == SDLK_RIGHTBRACKET) {
                    snapIncrement = std::min(5.0f, snapIncrement + 0.05f);
                    gizmo.setTranslateSnapIncrement(snapIncrement);
                    gizmo.setScaleSnapIncrement(std::max(0.01f, snapIncrement * 0.2f));
                    gizmo.setRotateSnapIncrement(std::max(1.0f, snapIncrement * 20.0f));
                    spdlog::info("Snap increment: {:.2f}", snapIncrement);
                }
            }

            const bool consumedByGizmo = gizmo.handleEvent(event, winW, winH);
            if (consumedByGizmo) {
                continue;
            }

            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
                orbiting = true;
                prevMouseX = event.button.x;
                prevMouseY = event.button.y;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT) {
                orbiting = false;
            } else if (event.type == SDL_EVENT_MOUSE_MOTION && orbiting) {
                const float dx = event.motion.x - prevMouseX;
                const float dy = event.motion.y - prevMouseY;
                prevMouseX = event.motion.x;
                prevMouseY = event.motion.y;

                orbitYaw -= dx * 0.25f;
                orbitPitch = std::clamp(orbitPitch - dy * 0.25f, -85.0f, 85.0f);
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                orbitDist = std::clamp(orbitDist - event.wheel.y * 0.35f, 2.0f, 20.0f);
            }
        }

        const float pitchRad = orbitPitch * DEG_TO_RAD;
        const float yawRad = orbitYaw * DEG_TO_RAD;
        const Vector3 camPos(
            focusPoint.getX() - std::sin(yawRad) * std::cos(pitchRad) * orbitDist,
            focusPoint.getY() + std::sin(pitchRad) * orbitDist,
            focusPoint.getZ() - std::cos(yawRad) * std::cos(pitchRad) * orbitDist
        );
        cameraEntity->setPosition(camPos);
        setLookAt(cameraEntity, focusPoint);

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq));
        prevCounter = nowCounter;

        gizmo.update();
        engine->update(dt);
        engine->render();
    }

    shutdown();
    return 0;
}
