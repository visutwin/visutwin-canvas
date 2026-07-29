// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Demonstrates dual-source blending, where the fragment shader emits a SECOND colour that the
// blend equation uses as a factor. The overlay computes `source0 + destination * source1`, so it
// tints differently depending on what is already in the framebuffer: over the black panel only
// the red `source0` survives, while over the white panel the green `source1` contribution is
// added as well.
//
// Port of upstream's test/dual-source-blending example.
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <iostream>
#include <memory>
#include <string>

#include <QuartzCore/QuartzCore.hpp>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/constants.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/shaderMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 600;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

// Flat-colour background panels. Constant output, no lighting or tonemapping, so the framebuffer
// contents underneath the overlay are exactly known.
static const char* kPanelShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexData {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct SceneData { float4x4 projViewMatrix; };

struct ModelData {
    float4x4 modelMatrix;
    float4x4 normalMatrix;
    float     normalSign;
    float3    _pad;
};

struct Varyings {
    float4 position [[position]];
};

vertex Varyings vertexShader(VertexData v [[stage_in]],
                             constant SceneData &scene [[buffer(1)]],
                             constant ModelData &model [[buffer(2)]])
{
    Varyings out;
    float4 clip = scene.projViewMatrix * (model.modelMatrix * float4(v.position, 1.0));
    clip.z = 0.5 * (clip.z + clip.w);   // GL [-1,1] -> Metal [0,1]
    out.position = clip;
    return out;
}

fragment float4 fragmentShader(Varyings in [[stage_in]])
{
    return float4(PANEL_COLOR, 1.0);
}
)MSL";

// The overlay. Two colour outputs on attachment 0: index 0 is the regular source colour, index 1
// is the secondary source the BLENDMODE_SRC1_* factors read.
static const char* kDualSourceShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexData {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct SceneData { float4x4 projViewMatrix; };

struct ModelData {
    float4x4 modelMatrix;
    float4x4 normalMatrix;
    float     normalSign;
    float3    _pad;
};

struct Varyings {
    float4 position [[position]];
};

// Dual-source output: both members target colour attachment 0, distinguished by index().
struct DualSourceOut {
    float4 color     [[color(0), index(0)]];
    float4 secondary [[color(0), index(1)]];
};

vertex Varyings vertexShader(VertexData v [[stage_in]],
                             constant SceneData &scene [[buffer(1)]],
                             constant ModelData &model [[buffer(2)]])
{
    Varyings out;
    float4 clip = scene.projViewMatrix * (model.modelMatrix * float4(v.position, 1.0));
    clip.z = 0.5 * (clip.z + clip.w);
    out.position = clip;
    return out;
}

fragment DualSourceOut fragmentShader(Varyings in [[stage_in]])
{
    DualSourceOut out;
    out.color     = float4(0.45, 0.02, 0.02, 0.0);   // source0 - added unconditionally
    out.secondary = float4(0.0, 0.85, 0.18, 1.0);    // source1 - scales the destination
    return out;
}
)MSL";

Entity* addQuad(const std::shared_ptr<Engine>& engine, Material* material,
    const Vector3& position, const Vector3& scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine.get());
    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setMaterial(material);
        render->setType("plane");
        render->setCastShadows(false);
        render->setReceiveShadows(false);
    }
    // "plane" lies in XZ; stand it up to face the camera.
    entity->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());
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
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    spdlog::info("*** VisuTwin Dual-Source Blending Example Started ***");

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow("VisuTwin Dual-Source Blending", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
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

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
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

    if (!graphicsDevice->supportsDualSourceBlending()) {
        spdlog::error("This device reports no dual-source blending support");
        shutdown();
        return -1;
    }

    // Two background panels: black on the left, white on the right. The colour is baked into
    // each shader so the framebuffer contents under the overlay are exactly known.
    const auto panelSource = [](const char* color) {
        return std::string("#define PANEL_COLOR float3") + color + "\n" + kPanelShaderSource;
    };

    auto blackMaterial = std::make_shared<ShaderMaterial>(
        graphicsDevice, "panel-black", "vertexShader", "fragmentShader",
        panelSource("(0.0, 0.0, 0.0)"));

    auto whiteMaterial = std::make_shared<ShaderMaterial>(
        graphicsDevice, "panel-white", "vertexShader", "fragmentShader",
        panelSource("(1.0, 1.0, 1.0)"));

    addQuad(engine, blackMaterial.get(), Vector3(-2.0f, 0.0f, 0.0f), Vector3(4.0f, 1.0f, 6.0f));
    addQuad(engine, whiteMaterial.get(), Vector3(2.0f, 0.0f, 0.0f), Vector3(4.0f, 1.0f, 6.0f));

    // The dual-source overlay, spanning both panels.
    //   colour = source0 * ONE + destination * source1
    auto overlayMaterial = std::make_shared<ShaderMaterial>(
        graphicsDevice, "dual-source-overlay", "vertexShader", "fragmentShader", kDualSourceShaderSource);

    auto blend = std::make_shared<BlendState>();
    blend->setEnabled(true);
    blend->setColorOp(BLENDEQUATION_ADD);
    blend->setColorSrcFactor(BLENDMODE_ONE);
    blend->setColorDstFactor(BLENDMODE_SRC1_COLOR);
    blend->setAlphaOp(BLENDEQUATION_ADD);
    blend->setAlphaSrcFactor(BLENDMODE_ZERO);
    blend->setAlphaDstFactor(BLENDMODE_ONE);
    overlayMaterial->setBlendState(blend);
    overlayMaterial->setTransparent(true);   // draw after the opaque panels

    spdlog::info("Overlay blend state reports dual-source: {}", blend->usesDualSourceBlending());

    addQuad(engine, overlayMaterial.get(), Vector3(0.0f, 0.0f, 0.1f), Vector3(6.0f, 1.0f, 3.0f));

    // Orthographic camera looking straight at the panels.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    auto* cameraComp = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>());
    if (cameraComp && cameraComp->camera()) {
        cameraComp->camera()->setProjection(ProjectionType::Orthographic);
        cameraComp->camera()->setOrthoHeight(3.5f);
        cameraComp->camera()->setClearColor(Color(0.02f, 0.02f, 0.02f, 1.0f));
    }
    camera->setLocalPosition(0.0f, 0.0f, 10.0f);
    engine->root()->addChild(camera);

    spdlog::info("Left half: destination is black, so only source0 (dark red) shows.");
    spdlog::info("Right half: destination is white, so source1 adds green on top. Esc quits.");

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();

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

        engine->update(dt);
        engine->render();
    }

    shutdown();
    return 0;
}
