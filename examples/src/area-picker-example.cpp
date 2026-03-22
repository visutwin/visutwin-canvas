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
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <unordered_set>
#include <vector>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/animation/animationComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/constants.h"
#include "framework/graphics/picker.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;
constexpr float PICKER_SCALE = 0.25f;
constexpr float CAMERA_ORBIT_RADIUS = 40.0f;

using namespace visutwin::canvas;

SDL_Window* window;
SDL_Renderer* renderer;

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

void setLookAt(Entity* camera, const Vector3& target)
{
    if (!camera) {
        return;
    }

    const Vector3 position = camera->position();
    const Vector3 lookDir = (target - position).normalized();
    const float pitchDeg = std::asin(std::clamp(lookDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yawDeg = std::atan2(-lookDir.getX(), -lookDir.getZ()) * RAD_TO_DEG;
    camera->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
}

Entity* createPrimitiveEntity(
    Engine* engine, const std::string& type, const Vector3& position, const Vector3& scale, StandardMaterial* material)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position);
    entity->setLocalScale(scale);

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType(type);
    }

    engine->root()->addChild(entity);
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
        "VisuTwin Area Picker", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    createOptions.registerComponentSystem<AnimationComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setSkyboxMip(2);
    scene->setSkyboxIntensity(0.1f);

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad texture");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    auto* light = new Entity();
    light->setEngine(engine.get());
    auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>());
    if (lightComponent) {
        lightComponent->setIntensity(1.5f);
        lightComponent->setType(LightType::LIGHTTYPE_DIRECTIONAL);
    }
    light->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(light);

    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    if (cameraComponent && cameraComponent->camera()) {
        cameraComponent->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
    }
    engine->root()->addChild(cameraEntity);

    std::vector<std::shared_ptr<StandardMaterial>> primitiveMaterials;
    primitiveMaterials.reserve(320);
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::uniform_real_distribution<float> scaleDistribution(1.0f, 2.0f);
    std::uniform_real_distribution<float> positionDistribution(-15.0f, 15.0f);
    std::uniform_int_distribution<int> shapeDistribution(0, 1);

    for (int i = 0; i < 300; ++i) {
        const bool useCylinder = shapeDistribution(rng) == 0;
        const std::string primitiveType = useCylinder ? "cylinder" : "sphere";
        const Vector3 position(
            positionDistribution(rng),
            positionDistribution(rng),
            positionDistribution(rng)
        );
        const float scale = scaleDistribution(rng);

        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(unit(rng), unit(rng), unit(rng), 1.0f));
        material->setGloss(0.6f);
        material->setMetalness(0.4f);
        material->setUseMetalness(true);
        primitiveMaterials.push_back(material);

        createPrimitiveEntity(engine.get(), primitiveType, position, Vector3(scale, scale, scale), material.get());
    }

    auto markerMaterial = std::make_shared<StandardMaterial>();
    markerMaterial->setEmissive(Color(0.0f, 1.0f, 0.0f, 1.0f));
    markerMaterial->setEmissiveIntensity(40.0f);
    auto* marker = createPrimitiveEntity(
        engine.get(), "sphere", Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f), markerMaterial.get()
    );
    if (auto* markerRender = marker->findComponent<RenderComponent>()) {
        markerRender->setLayers({LAYERID_UI});
    }
    primitiveMaterials.push_back(markerMaterial);

    Picker picker(engine.get(), static_cast<int>(WINDOW_WIDTH * PICKER_SCALE), static_cast<int>(WINDOW_HEIGHT * PICKER_SCALE), true);
    std::vector<int> pickerLayers = {LAYERID_WORLD};

    int mouseX = WINDOW_WIDTH / 2;
    int mouseY = WINDOW_HEIGHT / 2;
    std::optional<std::pair<int, int>> pendingWorldPick;

    struct PickRect
    {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        Color color = Color(1.0f, 0.02f, 0.58f, 1.0f);
    };
    std::vector<PickRect> rectangles = {
        {40, 40, 180, 140, Color(1.0f, 0.02f, 0.58f, 1.0f)},
        {280, 180, 220, 160, Color(0.0f, 1.0f, 1.0f, 1.0f)},
        {520, 80, 90, 90, Color(1.0f, 1.0f, 0.0f, 1.0f)}
    };

    std::vector<StandardMaterial*> highlightedMaterials;
    highlightedMaterials.reserve(512);

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
            } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
                mouseX = static_cast<int>(event.motion.x);
                mouseY = static_cast<int>(event.motion.y);
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                pendingWorldPick = std::make_pair(
                    static_cast<int>(event.button.x * PICKER_SCALE),
                    static_cast<int>(event.button.y * PICKER_SCALE)
                );
            }
        }

        const uint64_t currentCounter = SDL_GetPerformanceCounter();
        float deltaTime = static_cast<float>(currentCounter - prevCounter) / static_cast<float>(perfFreq);
        prevCounter = currentCounter;
        deltaTime = std::clamp(deltaTime, 0.0f, 0.1f);
        time += deltaTime * 0.1f;

        const Vector3 cameraPos(
            CAMERA_ORBIT_RADIUS * std::sin(time),
            0.0f,
            CAMERA_ORBIT_RADIUS * std::cos(time)
        );
        cameraEntity->setLocalPosition(cameraPos);
        setLookAt(cameraEntity, Vector3(0.0f, 0.0f, 0.0f));

        int windowW = 0;
        int windowH = 0;
        SDL_GetWindowSize(window, &windowW, &windowH);
        picker.resize(
            std::max(1, static_cast<int>(windowW * PICKER_SCALE)),
            std::max(1, static_cast<int>(windowH * PICKER_SCALE))
        );

        picker.prepare(cameraComponent, scene.get(), pickerLayers);

        for (auto* material : highlightedMaterials) {
            if (!material) {
                continue;
            }
            material->setEmissive(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissiveIntensity(1.0f);
        }
        highlightedMaterials.clear();

        std::unordered_set<StandardMaterial*> highlightedSet;

        auto highlightSelection = [&](const std::vector<MeshInstance*>& selection, const Color& color) {
            for (auto* meshInstance : selection) {
                if (!meshInstance) {
                    continue;
                }
                auto* material = dynamic_cast<StandardMaterial*>(meshInstance->material());
                if (!material) {
                    continue;
                }
                material->setEmissive(color);
                material->setEmissiveIntensity(30.0f);
                if (highlightedSet.insert(material).second) {
                    highlightedMaterials.push_back(material);
                }
            }
        };

        for (const auto& rect : rectangles) {
            const auto selection = picker.getSelection(
                static_cast<int>(rect.x * PICKER_SCALE),
                static_cast<int>(rect.y * PICKER_SCALE),
                std::max(1, static_cast<int>(rect.w * PICKER_SCALE)),
                std::max(1, static_cast<int>(rect.h * PICKER_SCALE))
            );
            highlightSelection(selection, rect.color);
        }

        const auto mouseSelection = picker.getSelectionSingle(
            static_cast<int>(mouseX * PICKER_SCALE),
            static_cast<int>(mouseY * PICKER_SCALE)
        );
        if (mouseSelection) {
            auto* material = dynamic_cast<StandardMaterial*>(mouseSelection->material());
            if (material) {
                material->setEmissive(Color(1.0f, 0.02f, 0.58f, 1.0f));
                material->setEmissiveIntensity(45.0f);
                if (highlightedSet.insert(material).second) {
                    highlightedMaterials.push_back(material);
                }
            }
        }

        if (pendingWorldPick.has_value()) {
            const auto [pickX, pickY] = *pendingWorldPick;
            const auto worldPoint = picker.getWorldPoint(pickX, pickY);
            if (worldPoint.has_value()) {
                marker->setLocalPosition(*worldPoint);
                marker->setLocalScale(0.2f, 0.2f, 0.2f);
            } else {
                marker->setLocalScale(0.0f, 0.0f, 0.0f);
            }
            pendingWorldPick.reset();
        }

        engine->update(deltaTime);
        engine->render();
    }

    shutdown();
    return 0;
}
