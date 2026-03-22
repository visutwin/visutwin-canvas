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
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "framework/engine.h"
#include "log.h"
#include "core/math/vector4.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

using namespace visutwin::canvas;

namespace
{
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;

    const std::string rootPath = ASSET_DIR;
    const auto checkerboard = std::make_unique<Asset>(
        "checkerboard",
        AssetType::TEXTURE,
        rootPath + "/textures/checkboard.png"
    );
    const auto courierFont = std::make_unique<Asset>(
        "courier-font",
        AssetType::FONT,
        rootPath + "/fonts/courier.json"
    );

    struct PlayerUi
    {
        Entity* worldEntity = nullptr;
        std::shared_ptr<StandardMaterial> worldMaterial;
        float angleDeg = 0.0f;
        float speedDegPerSec = 0.0f;
        float radius = 1.0f;
        float health = 1.0f;

        Entity* panelEntity = nullptr;
        ElementComponent* panelElement = nullptr;
        Entity* nameEntity = nullptr;
        ElementComponent* nameElement = nullptr;
        Entity* healthEntity = nullptr;
        ElementComponent* healthElement = nullptr;
        ButtonComponent* nameButton = nullptr;
        Entity* panelVisual = nullptr;
        Entity* healthVisual = nullptr;
        std::shared_ptr<StandardMaterial> panelVisualMaterial;
        std::shared_ptr<StandardMaterial> healthVisualMaterial;
        bool visible = false;
        int visibleStreak = 0;
        int hiddenStreak = 0;
        Vector3 lastScreenPos = Vector3(0.0f, 0.0f, 0.0f);
        bool hasLastScreenPos = false;
    };

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

    bool worldToScreenSpace(
        const Vector3& worldPosition,
        CameraComponent* cameraComponent,
        Entity* cameraEntity,
        ScreenComponent* screenComponent,
        Vector3& outScreen)
    {
        if (!cameraComponent || !cameraComponent->camera() || !cameraEntity || !screenComponent) {
            return false;
        }

        const Matrix4 view = cameraEntity->worldTransform().inverse();
        const Matrix4 proj = cameraComponent->camera()->projectionMatrix();
        const Vector3 viewPos = view.transformPoint(worldPosition);

        if (viewPos.getZ() >= 0.0f) {
            return false;
        }

        const Vector4 clip = proj * Vector4(viewPos.getX(), viewPos.getY(), viewPos.getZ(), 1.0f);
        if (std::abs(clip.getW()) < 1e-6f) {
            return false;
        }

        const float ndcX = clip.getX() / clip.getW();
        const float ndcY = clip.getY() / clip.getW();

        const Vector2 screenRes = screenComponent->resolution();
        const float sxPx = (ndcX * 0.5f + 0.5f) * screenRes.x;
        const float syPx = (1.0f - (ndcY * 0.5f + 0.5f)) * screenRes.y;

        const float scale = std::max(screenComponent->scale(), 1e-6f);
        outScreen = Vector3(sxPx / scale, syPx / scale, (-viewPos.getZ()) / scale);
        return true;
    }

    PlayerUi createPlayer(Engine* engine, Entity* screenEntity, int id, float startAngle, float speed, float radius)
    {
        PlayerUi player;
        player.angleDeg = startAngle;
        player.speedDegPerSec = speed;
        player.radius = radius;

        player.worldMaterial = std::make_shared<StandardMaterial>();
        player.worldMaterial->setDiffuse(Color(0.85f, 0.85f, 0.9f, 1.0f));
        player.worldEntity = createPrimitiveEntity(
            engine,
            "capsule",
            Vector3(0.0f, 0.5f, 0.0f),
            Vector3(0.5f, 0.5f, 0.5f),
            player.worldMaterial.get()
        );

        if (auto* body = static_cast<RigidBodyComponent*>(player.worldEntity->addComponent<RigidBodyComponent>())) {
            body->setType("static");
        }
        if (auto* col = static_cast<CollisionComponent*>(player.worldEntity->addComponent<CollisionComponent>())) {
            col->setType("capsule");
            col->setRadius(0.35f);
            col->setHeight(1.0f);
        }

        player.panelEntity = new Entity();
        player.panelEntity->setEngine(engine);
        player.panelElement = static_cast<ElementComponent*>(player.panelEntity->addComponent<ElementComponent>());
        if (player.panelElement) {
            player.panelElement->setType(ElementType::Image);
            player.panelElement->setPivot(Vector2(0.5f, 0.0f));
            player.panelElement->setAnchor(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            player.panelElement->setWidth(150.0f);
            player.panelElement->setHeight(50.0f);
            player.panelElement->setColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
            player.panelElement->setOpacity(0.35f);
        }
        screenEntity->addChild(player.panelEntity);

        player.nameEntity = new Entity();
        player.nameEntity->setEngine(engine);
        player.nameEntity->setLocalPosition(0.0f, 4.0f, 0.0f);
        player.nameElement = static_cast<ElementComponent*>(player.nameEntity->addComponent<ElementComponent>());
        if (player.nameElement) {
            player.nameElement->setType(ElementType::Text);
            player.nameElement->setPivot(Vector2(0.5f, 0.0f));
            player.nameElement->setWidth(130.0f);
            player.nameElement->setHeight(20.0f);
            player.nameElement->setFontSize(22);
            player.nameElement->setText("Player " + std::to_string(id));
            player.nameElement->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            player.nameElement->setHorizontalAlign(ElementHorizontalAlign::Center);
            player.nameElement->setWrapLines(false);
            player.nameElement->setUseInput(true);
        }
        player.nameButton = static_cast<ButtonComponent*>(player.nameEntity->addComponent<ButtonComponent>());
        if (player.nameButton) {
            player.nameButton->setImageEntity(player.nameEntity);
        }
        player.panelEntity->addChild(player.nameEntity);

        player.healthEntity = new Entity();
        player.healthEntity->setEngine(engine);
        player.healthEntity->setLocalPosition(0.0f, 32.0f, 0.0f);
        player.healthElement = static_cast<ElementComponent*>(player.healthEntity->addComponent<ElementComponent>());
        if (player.healthElement) {
            player.healthElement->setType(ElementType::Image);
            player.healthElement->setPivot(Vector2(0.5f, 0.0f));
            player.healthElement->setWidth(130.0f);
            player.healthElement->setHeight(10.0f);
            player.healthElement->setColor(Color(0.2f, 0.6f, 0.2f, 1.0f));
            player.healthElement->setOpacity(1.0f);
        }
        player.panelEntity->addChild(player.healthEntity);

        // Visual representation rendered by engine on UI layer (stable single-present path).
        player.panelVisualMaterial = std::make_shared<StandardMaterial>();
        player.panelVisualMaterial->setUseLighting(false);
        player.panelVisualMaterial->setUseSkybox(false);
        player.panelVisualMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        player.panelVisualMaterial->setEmissive(Color(0.0f, 0.0f, 0.0f, 1.0f));
        player.panelVisualMaterial->setOpacity(0.35f);
        player.panelVisualMaterial->setTransparent(true);
        player.panelVisualMaterial->setCullMode(CullMode::CULLFACE_NONE);
        player.panelVisual = createPrimitiveEntity(
            engine,
            "box",
            Vector3(0.0f, 0.0f, 0.0f),
            Vector3(150.0f, 50.0f, 0.2f),
            player.panelVisualMaterial.get(),
            {LAYERID_UI}
        );

        player.healthVisualMaterial = std::make_shared<StandardMaterial>();
        player.healthVisualMaterial->setUseLighting(false);
        player.healthVisualMaterial->setUseSkybox(false);
        player.healthVisualMaterial->setDiffuse(Color(0.2f, 0.6f, 0.2f, 1.0f));
        player.healthVisualMaterial->setEmissive(Color(0.2f, 0.6f, 0.2f, 1.0f));
        player.healthVisualMaterial->setTransparent(true);
        player.healthVisualMaterial->setCullMode(CullMode::CULLFACE_NONE);
        player.healthVisual = createPrimitiveEntity(
            engine,
            "box",
            Vector3(0.0f, -12.0f, 0.0f),
            Vector3(130.0f, 10.0f, 0.2f),
            player.healthVisualMaterial.get(),
            {LAYERID_UI}
        );

        return player;
    }

    bool isOccluded(const Vector3& cameraPos, const Vector3& targetPos, Entity* target, const RigidBodyComponentSystem* rb)
    {
        if (!target || !rb) {
            return false;
        }
        const auto hits = rb->raycastAll(cameraPos, targetPos);
        if (hits.empty()) {
            return false;
        }
        const float targetDistSq = (targetPos - cameraPos).lengthSquared();
        for (const auto& hit : hits) {
            if (!hit.entity) {
                continue;
            }
            if (hit.entity == target || hit.entity->isDescendantOf(target)) {
                continue;
            }
            const float hitDistSq = (hit.point - cameraPos).lengthSquared();
            // Treat as occluder only if it is clearly before target point.
            if (hitDistSq + 1e-4f < targetDistSq) {
                return true;
            }
        }
        return false;
    }
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
        "VisuTwin Screen Overlay Anchors (World-To-Screen)",
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

    auto elementInput = std::make_shared<ElementInput>();

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<CollisionComponentSystem>();
    createOptions.registerComponentSystem<RigidBodyComponentSystem>();
    createOptions.registerComponentSystem<ScreenComponentSystem>();
    createOptions.registerComponentSystem<ElementComponentSystem>();
    createOptions.registerComponentSystem<ButtonComponentSystem>();
    createOptions.elementInput = elementInput;

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setAmbientLight(0.3f, 0.3f, 0.32f);

    auto groundMaterial = std::make_shared<StandardMaterial>();
    if (const auto checkerResource = checkerboard->resource()) {
        groundMaterial->setDiffuseMap(std::get<Texture*>(*checkerResource));
    } else {
        groundMaterial->setDiffuse(Color(0.75f, 0.75f, 0.75f, 1.0f));
    }

    auto* ground = createPrimitiveEntity(
        engine.get(),
        "box",
        Vector3(0.0f, -0.5f, 0.0f),
        Vector3(50.0f, 1.0f, 50.0f),
        groundMaterial.get()
    );
    if (auto* body = static_cast<RigidBodyComponent*>(ground->addComponent<RigidBodyComponent>())) {
        body->setType("static");
    }
    if (auto* col = static_cast<CollisionComponent*>(ground->addComponent<CollisionComponent>())) {
        col->setType("box");
        col->setHalfExtents(Vector3(25.0f, 0.5f, 25.0f));
    }

    auto occluderMat = std::make_shared<StandardMaterial>();
    occluderMat->setDiffuse(Color(0.55f, 0.58f, 0.64f, 1.0f));
    auto* occluder = createPrimitiveEntity(
        engine.get(),
        "box",
        Vector3(0.0f, 1.25f, 0.0f),
        Vector3(1.5f, 2.5f, 0.6f),
        occluderMat.get()
    );
    if (auto* body = static_cast<RigidBodyComponent*>(occluder->addComponent<RigidBodyComponent>())) {
        body->setType("static");
    }
    if (auto* col = static_cast<CollisionComponent*>(occluder->addComponent<CollisionComponent>())) {
        col->setType("box");
        col->setHalfExtents(Vector3(0.75f, 1.25f, 0.3f));
    }

    auto* lightEntity = new Entity();
    lightEntity->setEngine(engine.get());
    auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    if (light) {
        light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        light->setIntensity(1.0f);
        light->setCastShadows(true);
        light->setShadowResolution(2048);
        light->setShadowDistance(16.0f);
        light->setShadowBias(0.2f);
        light->setShadowNormalBias(0.05f);
    }
    lightEntity->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(lightEntity);

    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    if (cameraComponent && cameraComponent->camera()) {
        cameraComponent->camera()->setClearColor(Color(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f));
    }
    cameraEntity->setLocalEulerAngles(-30.0f, 0.0f, 0.0f);
    cameraEntity->setLocalPosition(0.0f, 3.5f, 7.0f);
    cameraComponent->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX});
    engine->root()->addChild(cameraEntity);

    // Orthographic UI camera rendering only the UI layer.
    auto* uiCameraEntity = new Entity();
    uiCameraEntity->setEngine(engine.get());
    auto* uiCamera = static_cast<CameraComponent*>(uiCameraEntity->addComponent<CameraComponent>());
    if (uiCamera && uiCamera->camera()) {
        uiCamera->camera()->setProjection(ProjectionType::Orthographic);
        uiCamera->camera()->setOrthoHeight(static_cast<float>(WINDOW_HEIGHT) * 0.5f);
        uiCamera->camera()->setClearColorBuffer(false);
        // Render UI over world by clearing depth before UI pass.
        uiCamera->camera()->setClearDepthBuffer(true);
        uiCamera->camera()->setClearStencilBuffer(true);
        uiCamera->setLayers({LAYERID_UI});
    }
    uiCameraEntity->setLocalPosition(0.0f, 0.0f, 10.0f);
    engine->root()->addChild(uiCameraEntity);

    auto* screenEntity = new Entity();
    screenEntity->setEngine(engine.get());
    auto* screenComponent = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
    if (screenComponent) {
        screenComponent->setReferenceResolution(Vector2(1280.0f, 720.0f));
        screenComponent->setScreenSpace(true);
    }
    engine->root()->addChild(screenEntity);

    std::vector<PlayerUi> players;
    players.reserve(3);
    players.push_back(createPlayer(engine.get(), screenEntity, 1, 135.0f, 30.0f, 1.5f));
    players.push_back(createPlayer(engine.get(), screenEntity, 2, 65.0f, -18.0f, 1.0f));
    players.push_back(createPlayer(engine.get(), screenEntity, 3, 0.0f, 15.0f, 2.5f));

    FontResource* fontResource = nullptr;
    if (const auto fontRes = courierFont->resource(); fontRes.has_value() && std::holds_alternative<FontResource*>(*fontRes)) {
        fontResource = std::get<FontResource*>(*fontRes);
    }
    if (fontResource) {
        for (auto& player : players) {
            if (player.nameElement) {
                player.nameElement->setFontResource(fontResource);
            }
        }
    } else {
        spdlog::warn("Courier bitmap font was not loaded. Text labels will remain disabled.");
    }

    std::mt19937 rng(1337u);
    std::uniform_real_distribution<float> unit(0.15f, 1.0f);
    for (auto& player : players) {
        if (!player.nameButton) {
            continue;
        }
        player.nameButton->on("click", [nameEl = player.nameElement, mat = player.worldMaterial, &rng, unit]() mutable {
            const Color color(unit(rng), unit(rng), unit(rng), 1.0f);
            if (nameEl) {
                nameEl->setColor(color);
            }
            if (mat) {
                mat->setDiffuse(color);
            }
        });
    }

    auto* rigidbodySystem = dynamic_cast<RigidBodyComponentSystem*>(engine->systems()->getById("rigidbody"));
    bool occlusionEnabled = false;
    spdlog::info("World-To-Screen controls: ESC quit, left-click player name recolor, O toggle occlusion ({})",
        occlusionEnabled ? "on" : "off");

    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_O) {
                occlusionEnabled = !occlusionEnabled;
                spdlog::info("Occlusion gating: {}", occlusionEnabled ? "enabled" : "disabled");
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
                elementInput->handleMouseButtonDown(event.button.x, event.button.y);
            }
        }

        int windowW = 1;
        int windowH = 1;
        SDL_GetWindowSize(window, &windowW, &windowH);
        float uiWidth = static_cast<float>(windowW);
        float uiHeight = static_cast<float>(windowH);
        if (screenComponent) {
            screenComponent->updateScaleFromWindow(windowW, windowH);
            const float scale = std::max(screenComponent->scale(), 1e-6f);
            uiWidth = screenComponent->resolution().x / scale;
            uiHeight = screenComponent->resolution().y / scale;
        }
        if (uiCamera && uiCamera->camera()) {
            uiCamera->camera()->setOrthoHeight(uiHeight * 0.5f);
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(nowCounter - prevCounter) / static_cast<float>(perfFreq);
        prevCounter = nowCounter;

        for (size_t i = 0; i < players.size(); ++i) {
            auto& player = players[i];

            player.angleDeg += dt * player.speedDegPerSec;
            if (player.angleDeg > 360.0f) {
                player.angleDeg -= 360.0f;
            } else if (player.angleDeg < -360.0f) {
                player.angleDeg += 360.0f;
            }

            const float rad = player.angleDeg * DEG_TO_RAD;
            const float x = player.radius * std::sin(rad);
            const float z = player.radius * std::cos(rad);
            player.worldEntity->setLocalPosition(x, 0.5f, z);
            player.worldEntity->setLocalEulerAngles(0.0f, player.angleDeg + 90.0f, 0.0f);

            (void)i;
            player.health = 0.75f;
        }

        engine->update(dt);

        for (auto& player : players) {
            const Vector3 base = player.worldEntity->position();
            const Vector3 headWorld(base.getX(), base.getY() + 0.6f, base.getZ());

            Vector3 screenPos;
            const bool inFront = worldToScreenSpace(headWorld, cameraComponent, cameraEntity, screenComponent, screenPos);
            const bool inBounds = inFront &&
                screenPos.getX() >= 16.0f && screenPos.getX() <= uiWidth - 16.0f &&
                screenPos.getY() >= 16.0f && screenPos.getY() <= uiHeight - 16.0f;
            const bool occluded = occlusionEnabled && inBounds &&
                isOccluded(cameraEntity->position(), headWorld, player.worldEntity, rigidbodySystem);
            const bool desiredVisible = inBounds && screenPos.getZ() > 0.0f && !occluded;

            player.panelEntity->setEnabled(desiredVisible);
            if (player.nameEntity) {
                player.nameEntity->setEnabled(desiredVisible);
            }
            if (player.healthEntity) {
                player.healthEntity->setEnabled(desiredVisible);
            }
            if (player.panelVisual) {
                player.panelVisual->setEnabled(desiredVisible);
            }
            if (player.healthVisual) {
                player.healthVisual->setEnabled(desiredVisible);
            }
            if (desiredVisible) {
                player.panelEntity->setLocalPosition(screenPos.getX(), screenPos.getY(), 0.0f);

                const float worldX = screenPos.getX() - uiWidth * 0.5f;
                const float worldY = uiHeight * 0.5f - screenPos.getY();
                if (player.panelVisual) {
                    player.panelVisual->setLocalPosition(worldX, worldY, 0.0f);
                    player.panelVisual->setLocalScale(150.0f, 50.0f, 0.2f);
                }

                if (player.healthElement) {
                    const float healthW = 130.0f * std::clamp(player.health, 0.0f, 1.0f);
                    player.healthElement->setWidth(healthW);
                    if (player.healthVisual) {
                        player.healthVisual->setLocalScale(healthW, 10.0f, 1.0f);
                        player.healthVisual->setLocalPosition(worldX - 65.0f + healthW * 0.5f, worldY - 12.0f, 0.0f);
                    }
                }
            } else {
                // Prevent stale overlays in case enable-state propagation is delayed.
                player.panelEntity->setLocalPosition(-10000.0f, -10000.0f, 0.0f);
                if (player.panelVisual) {
                    player.panelVisual->setLocalPosition(-10000.0f, -10000.0f, 0.0f);
                }
                if (player.healthVisual) {
                    player.healthVisual->setLocalPosition(-10000.0f, -10000.0f, 0.0f);
                }
            }
        }

        elementInput->syncTextElements();

        // Single present path through engine renderer.
        engine->render();
    }

    shutdown();
    return 0;
}
