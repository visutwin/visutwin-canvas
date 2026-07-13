// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// LTC area-light demo: a warm rectangular light panel hovers over a glossy floor and
// a row of spheres with increasing roughness. The linearly-transformed-cosines
// evaluation produces the characteristic stretched panel reflection on the floor and
// progressively blurrier highlights across the sphere row. The panel slowly tilts
// (auto-demo) so the reflection sweep is visible; Space pauses/resumes.
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
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
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
        "VisuTwin LTC Area Light", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    scene->setAmbientLight(0.06f, 0.06f, 0.07f);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    camera->addComponent<CameraComponent>();
    // Low grazing view across the floor so the stretched panel reflection is visible.
    camera->setLocalPosition(0.0f, 1.5f, 8.5f);
    camera->setLocalEulerAngles(-8.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // Roughness-ladder floor: five reflective strips running toward the panel,
    // roughness 0.05 -> 0.75. The LTC inverse-transform LUT stretches and blurs the
    // panel reflection differently on each strip — the classic LTC demonstration.
    std::vector<std::shared_ptr<StandardMaterial>> stripMaterials;
    constexpr float stripRoughness[5] = {0.05f, 0.15f, 0.30f, 0.50f, 0.75f};
    for (int i = 0; i < 5; ++i) {
        auto material = std::make_shared<StandardMaterial>();
        material->setName("floor-strip-r" + std::to_string(i));
        material->setDiffuse(Color(0.05f, 0.05f, 0.06f, 1.0f));
        material->setMetalness(0.7f);
        material->setGlossInvert(true);
        material->setGloss(stripRoughness[i]);  // glossInvert: value is roughness
        stripMaterials.push_back(material);
        engine->root()->addChild(createEntity(
            engine.get(), material.get(), "plane",
            Vector3(-4.8f + 2.4f * static_cast<float>(i), 0.0f, 0.0f), Vector3(2.4f, 1.0f, 24.0f)
        ));
    }

    // A single diffuse sphere off to the side for a soft-shading reference.
    auto sphereMaterial = std::make_shared<StandardMaterial>();
    sphereMaterial->setName("reference-sphere");
    sphereMaterial->setDiffuse(Color(0.9f, 0.9f, 0.9f, 1.0f));
    sphereMaterial->setMetalness(0.0f);
    sphereMaterial->setGlossInvert(true);
    sphereMaterial->setGloss(0.6f);  // roughness
    engine->root()->addChild(createEntity(
        engine.get(), sphereMaterial.get(), "sphere", Vector3(5.5f, 0.55f, 2.0f), Vector3(1.1f, 1.1f, 1.1f)
    ));

    // The rect area light: a near-vertical warm billboard behind the scene facing the
    // camera. Entity -Y is the emission direction, X is the rect width axis.
    auto* areaLight = new Entity();
    areaLight->setEngine(engine.get());
    auto* lightComponent = static_cast<LightComponent*>(areaLight->addComponent<LightComponent>());
    if (lightComponent) {
        lightComponent->setType(LightType::LIGHTTYPE_AREA_RECT);
        lightComponent->setColor(Color(1.0f, 0.7f, 0.35f, 1.0f));
        lightComponent->setIntensity(0.8f);
        lightComponent->setRange(30.0f);
        lightComponent->setAreaWidth(9.6f);
        lightComponent->setAreaHeight(1.6f);
    }
    areaLight->setLocalPosition(0.0f, 1.9f, -3.0f);
    engine->root()->addChild(areaLight);

    // Emissive slab so the panel itself is visible where the light comes from.
    auto panelMaterial = std::make_shared<StandardMaterial>();
    panelMaterial->setName("panel-emissive");
    panelMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
    panelMaterial->setEmissive(Color(1.0f, 0.7f, 0.35f, 1.0f));
    panelMaterial->setEmissiveIntensity(3.0f);
    auto* panel = createEntity(
        engine.get(), panelMaterial.get(), "box", Vector3(0.0f, 0.0f, 0.0f), Vector3(9.6f, 0.05f, 1.6f)
    );
    areaLight->addChild(panel);

    spdlog::info("LTC area light: 9.6m x 1.6m warm panel over floor strips roughness 0.05 -> 0.75");
    spdlog::info("Auto-cycles shape rect -> disk -> sphere every 4s. Keys: Space = pause/resume tilt, Esc = quit");

    // Shape cycle: rect -> disk -> sphere (disk inscribed in the panel quad;
    // sphere radius = max half-extent). The emissive slab is resized to match.
    const auto applyShape = [&](const int shapeIdx) {
        static const char* names[3] = {"RECT", "DISK", "SPHERE"};
        if (!lightComponent) return;
        lightComponent->setAreaShape(static_cast<AreaLightShape>(shapeIdx));
        if (shapeIdx == 2) {
            panel->setLocalScale(4.8f, 4.8f, 4.8f);  // sphere: uniform blob
        } else {
            panel->setLocalScale(9.6f, 0.05f, 1.6f);
        }
        spdlog::info("Area light shape: {}", names[shapeIdx]);
    };
    applyShape(0);

    bool running = true;
    bool animate = true;
    float animTime = 0.0f;
    float shapeTimer = 0.0f;
    int shapeIdx = 0;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    const auto poseLight = [&](const float t) {
        // Slow tilt oscillation around X: sweeps the reflection toward/away from camera.
        const float tilt = 82.0f + 8.0f * std::sin(t * 0.5f);
        areaLight->setLocalEulerAngles(tilt, 0.0f, 0.0f);
    };
    poseLight(0.0f);

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_SPACE) {
                animate = !animate;
                spdlog::info("Tilt {}", animate ? "resumed" : "paused");
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        if (animate) {
            animTime += static_cast<float>(dtSeconds);
            poseLight(animTime);
        }

        shapeTimer += static_cast<float>(dtSeconds);
        if (shapeTimer >= 4.0f) {
            shapeTimer = 0.0f;
            shapeIdx = (shapeIdx + 1) % 3;
            applyShape(shapeIdx);
        }

        engine->update(static_cast<float>(dtSeconds));
        engine->render();
    }

    shutdown();
    return 0;
}
