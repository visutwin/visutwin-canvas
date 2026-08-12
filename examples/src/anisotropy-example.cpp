// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Anisotropic specular demo (mirrors PlayCanvas materials/material-anisotropic).
//
// An 11 x 6 grid of metallic procedural spheres lit by the helipad env atlas plus
// a single directional light, matching upstream's sweeps exactly:
//   * X axis (columns): anisotropy 0 -> 1 (x / (NUM_SPHERES_X - 1)). At 0 the
//     highlight is a round GGX blob; as it grows the highlight stretches into a
//     brushed-metal streak.
//   * Z axis (rows): gloss 0 -> 1 (z / (NUM_SPHERES_Z - 1)) — upstream labels this
//     axis "Roughness" even though it sets gloss.
//
// Upstream also sets material.enableGGXSpecular; this engine has no equivalent
// flag because the anisotropic path is gated automatically on a non-zero
// anisotropy value (programLibrary.cpp: options.anisotropy = anisotropy() != 0),
// so the leftmost column renders isotropic exactly as upstream's does.
//
// The two axis labels ("Anisotropy", "Roughness") lie flat on the ground plane as
// WORLD-SPACE text: a Text element with no ScreenComponent ancestor is parented to
// its entity and drawn on the world layer with depth testing, so it follows the
// entity's full transform. setFontSize takes an int, so the em size is set coarsely
// (64) and scaled down on the entity; the element's HEIGHT must equal fontSize or
// the line is parked half a box-height above the origin.
//
// Orbit camera (CameraControls) starts at the upstream camera pose.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "../cameraControls.h"
#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/input/elementInput.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

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

// Bitmap font for the two world-space axis labels (upstream uses arial.json too).
const auto labelFont = std::make_unique<Asset>(
    "arial-font",
    AssetType::FONT,
    rootPath + "/fonts/arial.json"
);

// Grid dimensions and spacing match upstream material-anisotropic exactly.
constexpr int NUM_SPHERES_X = 11;  // anisotropy 0 .. 1
constexpr int NUM_SPHERES_Z = 6;   // gloss 0 .. 1
constexpr float SPACING = 1.0f;

int main()
{
    log::init();
    log::set_level_debug();

    window = nullptr;
    renderer = nullptr;

    const auto shutdown = []() {
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Anisotropic Specular", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) { std::cerr << "SDL Window Creation Failed" << std::endl; shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { std::cerr << "SDL Renderer Creation Failed" << std::endl; shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { std::cerr << "Unable to get render Metal layer" << std::endl; shutdown(); return -1; }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) { std::cerr << "Unable to create graphics device" << std::endl; shutdown(); return -1; }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<ElementComponentSystem>();
    auto elementInput = std::make_shared<ElementInput>();
    createOptions.elementInput = elementInput;
    createOptions.registerComponentSystem<ScriptComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    // Upstream sets only these — no ambient, no exposure or skybox-intensity
    // overrides — so the env atlas alone lights the spheres.
    scene->setToneMapping(TONEMAP_ACES);
    scene->setSkyboxMip(1);

    const auto helipadResource = helipad->resource();
    if (!helipadResource) {
        spdlog::error("Failed to load helipad env atlas");
        shutdown();
        return -1;
    }
    scene->setEnvAtlas(std::get<Texture*>(*helipadResource));

    // Single directional light (as upstream: base euler +90 X, -75 Y).
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lightComponent = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lightComponent->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
    }
    light->setLocalEulerAngles(90.0f, -75.0f, 0.0f);
    engine->root()->addChild(light);

    // Keep material lifetimes alive for the app duration.
    std::vector<std::shared_ptr<StandardMaterial>> materials;
    materials.reserve(NUM_SPHERES_X * NUM_SPHERES_Z);

    for (int iz = 0; iz < NUM_SPHERES_Z; ++iz) {
        // gloss = z / (NUM_SPHERES_Z - 1), i.e. 0 .. 1 (upstream's "Roughness" axis).
        const float gloss = NUM_SPHERES_Z > 1
            ? static_cast<float>(iz) / static_cast<float>(NUM_SPHERES_Z - 1)
            : 0.0f;

        for (int ix = 0; ix < NUM_SPHERES_X; ++ix) {
            // anisotropy = x / (NUM_SPHERES_X - 1), i.e. 0 .. 1 (upstream's sweep).
            const float aniso = NUM_SPHERES_X > 1
                ? static_cast<float>(ix) / static_cast<float>(NUM_SPHERES_X - 1)
                : 0.0f;

            auto material = std::make_shared<StandardMaterial>();
            material->setUseMetalness(true);
            material->setMetalness(1.0f);
            material->setGloss(gloss);
            material->setAnisotropy(aniso);
            materials.push_back(material);

            auto* sphere = new Entity();
            sphere->setEngine(engine.get());
            sphere->setLocalPosition(
                (static_cast<float>(ix) - (NUM_SPHERES_X - 1) * 0.5f) * SPACING,
                0.0f,
                (static_cast<float>(iz) - (NUM_SPHERES_Z - 1) * 0.5f) * SPACING
            );
            sphere->setLocalScale(0.7f, 0.7f, 0.7f);
            if (auto* render = static_cast<RenderComponent*>(sphere->addComponent<RenderComponent>())) {
                render->setMaterial(material.get());
                render->setType("sphere");
            }
            engine->root()->addChild(sphere);
        }
    }

    // Upstream's two axis labels, lying flat on the ground plane. These are
    // WORLD-SPACE text: the entities have no ScreenComponent ancestor, so the
    // element system parents the glyph mesh to the entity and renders it on the
    // world layer with depth testing, following the entity's full transform.
    //
    // setFontSize takes an int (min 1), so upstream's 0.5 em is expressed as
    // fontSize 1 on an entity scaled by 0.5 — the same size in world units.
    // Text is laid out from `yTop = (1 - pivot.y) * height` and flows DOWN, so a
    // single line is centred on the entity origin only when height == fontSize.
    // Leaving the default height (50) parks the label 25 mesh units above the
    // scene. Width is irrelevant here: with centre pivot the x offset works out
    // to -lineWidth/2 regardless, and wrapping is off.
    //
    // fontSize is an int, so size is set coarsely and scaled down: 64 mesh units
    // per em on a 0.5/64-scaled entity gives upstream's 0.5 world units per em.
    constexpr int LABEL_FONT_SIZE = 64;
    constexpr float LABEL_EM_WORLD = 0.5f;
    constexpr float LABEL_SCALE = LABEL_EM_WORLD / static_cast<float>(LABEL_FONT_SIZE);

    FontResource* labelFontResource = nullptr;
    if (const auto fontRes = labelFont->resource();
        fontRes.has_value() && std::holds_alternative<FontResource*>(*fontRes)) {
        labelFontResource = std::get<FontResource*>(*fontRes);
    }
    if (labelFontResource) {
        const auto createLabel = [&](const std::string& text, const Vector3& position,
                                     const Vector3& eulerAngles) {
            auto* label = new Entity();
            label->setEngine(engine.get());
            if (auto* element = static_cast<ElementComponent*>(
                    label->addComponent<ElementComponent>())) {
                element->setType(ElementType::Text);
                element->setText(text);
                element->setFontResource(labelFontResource);
                element->setFontSize(LABEL_FONT_SIZE);
                element->setHeight(static_cast<float>(LABEL_FONT_SIZE));
                element->setWidth(static_cast<float>(LABEL_FONT_SIZE) * 16.0f);
                element->setPivot(Vector2(0.5f, 0.5f));
                element->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            }
            label->setLocalPosition(position);
            label->setLocalEulerAngles(eulerAngles.getX(), eulerAngles.getY(),
                eulerAngles.getZ());
            label->setLocalScale(LABEL_SCALE, LABEL_SCALE, LABEL_SCALE);
            engine->root()->addChild(label);
        };

        createLabel("Anisotropy",
            Vector3(0.0f, 0.0f, (NUM_SPHERES_Z + 1) * 0.5f * SPACING),
            Vector3(-90.0f, 0.0f, 0.0f));
        createLabel("Roughness",
            Vector3(-(NUM_SPHERES_X + 1) * 0.5f * SPACING, 0.0f, 0.0f),
            Vector3(-90.0f, 90.0f, 0.0f));
    } else {
        spdlog::warn("Label font failed to load — axis labels will be missing");
    }

    // Camera pose copied from upstream: translate(0, 9, 9) + rotate(-48, 0, 0).
    // CameraControls derives its orbit state FROM this pose rather than moving
    // the camera, so the default framing matches while orbiting still works.
    // (Upstream's -48 deg pitch is ~3 deg off looking straight at the origin;
    // the controls settle on the exact look-at, which is visually identical.)
    const Vector3 focusPoint(0.0f, 0.0f, 0.0f);
    const float gridRadius = std::max(NUM_SPHERES_X, NUM_SPHERES_Z) * SPACING * 0.5f;

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    camera->addComponent<ScriptComponent>();
    camera->setPosition(Vector3(0.0f, 9.0f, 9.0f));
    camera->setLocalEulerAngles(-48.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);
    (void)cameraComponent;

    auto* cameraControls = camera->script()->create<CameraControls>();
    cameraControls->setFocusPoint(focusPoint);
    cameraControls->setEnableFly(false);
    cameraControls->setMoveSpeed(2.0f * gridRadius);
    cameraControls->setMoveFastSpeed(4.0f * gridRadius);
    cameraControls->setMoveSlowSpeed(gridRadius);
    cameraControls->storeResetState();

    spdlog::info("Anisotropic specular: {}x{} metallic sphere grid (upstream material-anisotropic parity).", NUM_SPHERES_X, NUM_SPHERES_Z);
    spdlog::info("Columns sweep anisotropy 0 -> 1; rows sweep gloss 0 -> 1.");
    spdlog::info("Watch the round GGX highlight stretch into a brushed-metal streak toward the right.");
    spdlog::info("Orbit: LMB/RMB orbit, Shift/MMB pan, Wheel/Pinch zoom, F focus, R reset, Esc quit.");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F && cameraControls) {
                cameraControls->focus(focusPoint, std::max(gridRadius * 1.6f, 6.0f));
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_R && cameraControls) {
                cameraControls->reset();
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && cameraControls) {
                cameraControls->addZoomInput(event.wheel.y);
            } else if (event.type == SDL_EVENT_PINCH_UPDATE && cameraControls) {
                const float pinchDelta = (event.pinch.scale - 1.0f) * 10.0f;
                cameraControls->addZoomInput(pinchDelta);
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const double dtSeconds = static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq);
        prevCounter = nowCounter;

        engine->update(static_cast<float>(dtSeconds));
        elementInput->syncTextElements();
        engine->render();
    }

    shutdown();
    return 0;
}
