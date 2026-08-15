// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Area picker demo (parity with upstream graphics/area-picker): 300 random
// metallic primitives in a 30-unit box, lit only by the dim helipad env atlas.
// A quarter-resolution Picker renders mesh ids offscreen; every frame four
// screen areas are scanned — a tall yellow rectangle, a wide cyan strip, a
// tiny magenta square and a 1x1 red probe at the mouse — and the materials
// inside glow with the area's color (bloom makes them read as highlights).
// Pink outlines mark the areas on screen. Left-click places a green marker at
// the picked world point; right-click stops the auto-orbit and hands over to
// interactive camera controls.
//
// DEVIATIONS: upstream draws the outlines with app.drawLines and previews the
// picker's color/depth buffers with app.drawTexture — this engine has neither,
// so outlines are camera-parented unlit rods on the IMMEDIATE layer (ViewCube
// pattern) and the buffer previews are skipped.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <unordered_set>
#include <vector>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
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

// Screen-space rectangle outline: four thin unlit rods on the IMMEDIATE layer,
// parented to the camera and positioned in camera-local space at a fixed
// distance so they stay glued to the given window-pixel rectangle
// (upstream: camera.screenToWorld + app.drawLines).
class RectOutline
{
public:
    RectOutline(Engine* engine, Entity* cameraEntity, CameraComponent* cameraComponent)
        : _cameraEntity(cameraEntity), _cameraComponent(cameraComponent)
    {
        _material = std::make_shared<StandardMaterial>();
        _material->setUseLighting(false);
        _material->setDiffuse(Color(1.0f, 0.02f, 0.58f, 1.0f)); // upstream pink
        auto depthState = std::make_shared<DepthState>();
        depthState->setDepthTest(false);
        depthState->setDepthWrite(false);
        _material->setDepthState(depthState);

        for (auto& rod : _rods) {
            auto* entity = new Entity();
            entity->setEngine(engine);
            if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
                render->setLayers({LAYERID_IMMEDIATE});
                render->setMaterial(_material.get());
                render->setType("box");
                render->setCastShadows(false);
                render->setReceiveShadows(false);
            }
            cameraEntity->addChild(entity);
            rod = entity;
        }
    }

    // Rectangle in window-pixel coordinates (origin top-left).
    void update(const float x, const float y, const float w, const float h,
        const float windowW, const float windowH) const
    {
        if (!_cameraComponent || !_cameraComponent->camera() || windowW <= 0.0f || windowH <= 0.0f) {
            return;
        }
        constexpr float distance = 1.0f;   // camera-local plane the outline lives on
        const float fovRad = _cameraComponent->camera()->fov() * DEG_TO_RAD;
        const float halfH = distance * std::tan(fovRad * 0.5f);
        const float halfW = halfH * (windowW / windowH);

        // Window pixels -> camera-local units on the z = -distance plane.
        const auto toLocalX = [&](const float px) { return (px / windowW - 0.5f) * 2.0f * halfW; };
        const auto toLocalY = [&](const float py) { return (0.5f - py / windowH) * 2.0f * halfH; };

        const float x0 = toLocalX(x), x1 = toLocalX(x + w);
        const float y0 = toLocalY(y), y1 = toLocalY(y + h);
        const float t = 2.0f * (2.0f * halfH / windowH);  // ~2px rod thickness

        const auto place = [&](Entity* rod, const float cx, const float cy, const float sx, const float sy) {
            rod->setLocalPosition(cx, cy, -distance);
            rod->setLocalScale(std::max(sx, t), std::max(sy, t), t);
        };
        place(_rods[0], (x0 + x1) * 0.5f, y0, x1 - x0, t);  // top
        place(_rods[1], (x0 + x1) * 0.5f, y1, x1 - x0, t);  // bottom
        place(_rods[2], x0, (y0 + y1) * 0.5f, t, y0 - y1);  // left
        place(_rods[3], x1, (y0 + y1) * 0.5f, t, y0 - y1);  // right
    }

private:
    Entity* _cameraEntity = nullptr;
    CameraComponent* _cameraComponent = nullptr;
    std::shared_ptr<StandardMaterial> _material;
    std::array<Entity*, 4> _rods{};
};

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
        "Area Picker", WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
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
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    // Upstream scene: dim skydome only — no analytical lights. Highlighted
    // objects glow via emissive + bloom.
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

    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    cameraEntity->addComponent<ScriptComponent>();
    if (cameraComponent && cameraComponent->camera()) {
        cameraComponent->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
    }
    // Bloom (upstream CameraFrame bloom intensity 0.01).
    if (cameraComponent) {
        auto rendering = cameraComponent->rendering();
        rendering.bloomIntensity = 0.01f;
        cameraComponent->setRendering(rendering);
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

    // Green marker for the picked world point (upstream emissiveIntensity 100).
    // Lives on the UI layer so the WORLD-layer picker never sees it; hidden by
    // zero scale until the first successful pick.
    auto markerMaterial = std::make_shared<StandardMaterial>();
    markerMaterial->setEmissive(Color(0.0f, 1.0f, 0.0f, 1.0f));
    markerMaterial->setEmissiveIntensity(100.0f);
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

    // Outline rods for the four areas (all pink, like upstream drawRectangle).
    std::vector<std::unique_ptr<RectOutline>> outlines;
    for (int i = 0; i < 4; ++i) {
        outlines.push_back(std::make_unique<RectOutline>(engine.get(), cameraEntity, cameraComponent));
    }

    std::vector<StandardMaterial*> highlightedMaterials;
    highlightedMaterials.reserve(512);

    // Auto-orbit until the first right-click hands control to CameraControls.
    bool autoRotate = true;
    CameraControls* cameraControls = nullptr;

    spdlog::info("Area picker: yellow/cyan/magenta areas + red mouse probe highlight picked objects.");
    spdlog::info("LMB = pick world point (green marker), RMB = take over camera, Esc = quit.");

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
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
                if (autoRotate) {
                    autoRotate = false;
                    // CameraControls attaches at the camera's current pose (no jump).
                    cameraControls = cameraEntity->script()->create<CameraControls>();
                    cameraControls->setFocusPoint(Vector3(0.0f, 0.0f, 0.0f));
                    cameraControls->setEnableFly(false);
                    spdlog::info("Camera handed over to interactive controls");
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            }
        }

        const uint64_t currentCounter = SDL_GetPerformanceCounter();
        float deltaTime = static_cast<float>(currentCounter - prevCounter) / static_cast<float>(perfFreq);
        prevCounter = currentCounter;
        deltaTime = std::clamp(deltaTime, 0.0f, 0.1f);

        if (autoRotate) {
            time += deltaTime * 0.1f;
            cameraEntity->setLocalPosition(Vector3(
                CAMERA_ORBIT_RADIUS * std::sin(time),
                0.0f,
                CAMERA_ORBIT_RADIUS * std::cos(time)
            ));
            setLookAt(cameraEntity, Vector3(0.0f, 0.0f, 0.0f));
        }

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

        // Upstream areas: tall yellow rect, wide cyan strip, tiny magenta
        // square (proportional positions), and a 1x1 red probe at the mouse.
        struct PickArea
        {
            float x, y, w, h;
            Color color;
        };
        const float fw = static_cast<float>(windowW);
        const float fh = static_cast<float>(windowH);
        const std::array<PickArea, 4> areas = {{
            {fw * 0.3f, fh * 0.3f, 100.0f, 200.0f, Color(1.0f, 1.0f, 0.0f, 1.0f)},
            {fw * 0.6f, fh * 0.7f, 200.0f, 20.0f, Color(0.0f, 1.0f, 1.0f, 1.0f)},
            {fw * 0.8f, fh * 0.3f, 5.0f, 5.0f, Color(1.0f, 0.0f, 1.0f, 1.0f)},
            {static_cast<float>(mouseX), static_cast<float>(mouseY), 1.0f, 1.0f, Color(1.0f, 0.0f, 0.0f, 1.0f)},
        }};

        for (size_t i = 0; i < areas.size(); ++i) {
            const auto& area = areas[i];
            outlines[i]->update(area.x, area.y, area.w, area.h, fw, fh);

            const auto selection = picker.getSelection(
                static_cast<int>(area.x * PICKER_SCALE),
                static_cast<int>(area.y * PICKER_SCALE),
                std::max(1, static_cast<int>(area.w * PICKER_SCALE)),
                std::max(1, static_cast<int>(area.h * PICKER_SCALE))
            );
            highlightSelection(selection, area.color);
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
