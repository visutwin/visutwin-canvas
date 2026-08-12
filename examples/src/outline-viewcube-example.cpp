// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// OutlineRenderer + ViewCube demo: three objects on a floor; the colored selection
// outline auto-cycles between them, and a world-axis view cube sits in the top-right
// corner. Clicking a view-cube handle (or pressing X/Y/Z) aligns the camera with that
// world axis. Space pauses/resumes the outline auto-cycle.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <algorithm>
#include <SDL3/SDL.h>
#include <cmath>
#include <memory>

#include <QuartzCore/QuartzCore.hpp>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "framework/extras/outlineRenderer.h"
#include "framework/extras/viewCube.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

Entity* createEntity(Engine* engine, Material* material, const char* type,
    const Vector3& position, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType(type);
    }
    return entity;
}

void setLookAt(Entity* camera, const Vector3& from, const Vector3& to)
{
    camera->setLocalPosition(from.getX(), from.getY(), from.getZ());
    const Vector3 dir = (to - from).normalized();
    const float pitch = std::asin(std::clamp(dir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yaw = std::atan2(-dir.getX(), -dir.getZ()) * RAD_TO_DEG;
    camera->setLocalEulerAngles(pitch, yaw, 0.0f);
}

int main()
{
    log::init();
    log::set_level_debug();

    window = nullptr;
    renderer = nullptr;

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
        "Outline + ViewCube", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setToneMapping(TONEMAP_ACES);
    scene->setAmbientLight(0.25f, 0.25f, 0.28f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    setLookAt(camera, Vector3(0.0f, 3.0f, 8.0f), Vector3(0.0f, 0.5f, 0.0f));
    engine->root()->addChild(camera);

    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
        lightComponent->setIntensity(1.2f);
    }
    light->setLocalEulerAngles(45.0f, -35.0f, 0.0f);
    engine->root()->addChild(light);

    // Floor + three objects.
    auto floorMaterial = std::make_shared<StandardMaterial>();
    floorMaterial->setDiffuse(Color(0.4f, 0.4f, 0.43f, 1.0f));
    floorMaterial->setGlossInvert(true);
    floorMaterial->setGloss(0.85f);
    engine->root()->addChild(createEntity(
        engine.get(), floorMaterial.get(), "plane", Vector3(0.0f, -0.5f, 0.0f), Vector3(20.0f, 1.0f, 20.0f)
    ));

    const char* types[3] = {"box", "sphere", "capsule"};
    const Color objectColors[3] = {
        Color(0.7f, 0.55f, 0.35f, 1.0f), Color(0.4f, 0.55f, 0.7f, 1.0f), Color(0.5f, 0.65f, 0.45f, 1.0f)
    };
    Entity* objects[3];
    std::vector<std::shared_ptr<StandardMaterial>> objectMaterials;
    for (int i = 0; i < 3; ++i) {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(objectColors[i]);
        material->setGlossInvert(true);
        material->setGloss(0.5f);
        objectMaterials.push_back(material);
        objects[i] = createEntity(
            engine.get(), material.get(), types[i],
            Vector3(-2.6f + 2.6f * static_cast<float>(i), 0.35f, 0.0f), Vector3(1.4f, 1.4f, 1.4f)
        );
        engine->root()->addChild(objects[i]);
    }

    // Extras: outline renderer + view cube.
    auto outline = std::make_unique<OutlineRenderer>(engine.get());
    auto viewCube = std::make_unique<ViewCube>(engine.get());

    const Color outlineColors[3] = {
        Color(1.0f, 0.55f, 0.0f, 1.0f), Color(0.0f, 0.9f, 1.0f, 1.0f), Color(1.0f, 0.2f, 0.7f, 1.0f)
    };
    int outlinedIndex = 0;
    const auto selectOutlined = [&](const int index) {
        outline->removeAllEntities();
        outline->addEntity(objects[index], outlineColors[index]);
        spdlog::info("Outlined object {} ({})", index, types[index]);
    };
    selectOutlined(0);

    // Camera alignment target driven by the view cube.
    const float orbitRadius = 8.5f;
    const auto alignCamera = [&](const Vector3& axis) {
        const Vector3 from = axis * orbitRadius + Vector3(0.0f, 0.5f, 0.0f);
        setLookAt(camera, from, Vector3(0.0f, 0.5f, 0.0f));
        spdlog::info("Camera aligned to axis ({}, {}, {})", axis.getX(), axis.getY(), axis.getZ());
    };
    viewCube->on(ViewCube::EVENT_CAMERAALIGN, [&alignCamera](const Vector3& axis) {
        alignCamera(axis);
    });

    spdlog::info("Keys: X/Y/Z = align camera, click view cube handles, Space = pause cycle, Esc = quit");

    bool running = true;
    bool autoCycle = true;
    float cycleTimer = 0.0f;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                autoCycle = !autoCycle;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_X) {
                alignCamera(Vector3(1.0f, 0.0f, 0.0f));
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_Y) {
                alignCamera(Vector3(0.0f, 1.0f, 0.0f));
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_Z) {
                alignCamera(Vector3(0.0f, 0.0f, 1.0f));
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                int windowWidth = 0, windowHeight = 0;
                SDL_GetWindowSize(window, &windowWidth, &windowHeight);
                viewCube->onClick(event.button.x, event.button.y,
                    static_cast<float>(windowWidth), static_cast<float>(windowHeight), camera);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (autoCycle) {
            cycleTimer += static_cast<float>(dtSeconds);
            if (cycleTimer >= 3.0f) {
                cycleTimer = 0.0f;
                outlinedIndex = (outlinedIndex + 1) % 3;
                selectOutlined(outlinedIndex);
            }
        }

        viewCube->update(camera);
        outline->frameUpdate(camera);

        // One-shot picking self-test (~1.5 s in): scan the top-right corner through
        // onClick until a handle reports a hit — exercises the ray/sphere math and the
        // camera:align event end-to-end without needing a real mouse click.
        static bool pickTested = false;
        static float pickTimer = 0.0f;
        pickTimer += static_cast<float>(dtSeconds);
        if (!pickTested && pickTimer > 1.5f) {
            pickTested = true;
            int windowWidth = 0, windowHeight = 0;
            SDL_GetWindowSize(window, &windowWidth, &windowHeight);
            bool hit = false;
            for (int sy = 0; sy < windowHeight / 2 && !hit; sy += 6) {
                for (int sx = windowWidth / 2; sx < windowWidth && !hit; sx += 6) {
                    if (const auto axis = viewCube->onClick(static_cast<float>(sx), static_cast<float>(sy),
                            static_cast<float>(windowWidth), static_cast<float>(windowHeight), camera)) {
                        spdlog::info("Pick self-test HIT at ({}, {}) axis ({}, {}, {})",
                            sx, sy, axis->getX(), axis->getY(), axis->getZ());
                        hit = true;
                    }
                }
            }
            if (!hit) {
                spdlog::warn("Pick self-test: no handle hit in scan");
            }
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    outline.reset();
    viewCube.reset();
    shutdown();
    return 0;
}
