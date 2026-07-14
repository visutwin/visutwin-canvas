// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Custom shader material example — mirrors PlayCanvas shaders/shader-toon.
// Demonstrates ShaderMaterial: a user-supplied Metal vertex + fragment shader
// (bypassing the standard PBR chunk pipeline) implementing cel/toon shading —
// quantised N·L bands plus a warm/cool gradient. Three primitives rotate so the
// banding sweeps across their surfaces.
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <memory>
#include <vector>

#include <QuartzCore/QuartzCore.hpp>

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/constants.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"
#include "scene/materials/shaderMaterial.h"

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;

SDL_Window* window;
SDL_Renderer* renderer;

using namespace visutwin::canvas;

// Self-contained toon shader. It declares the same vertex-input attributes and
// per-draw uniform buffers the forward pass binds (SceneData @1, ModelData @2),
// so the standard mesh vertex layout feeds it directly.
static const char* kToonShaderSource = R"MSL(
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
    float3 worldNormal;
};

vertex Varyings vertexShader(VertexData v [[stage_in]],
                             constant SceneData &scene [[buffer(1)]],
                             constant ModelData &model [[buffer(2)]])
{
    Varyings out;
    float4 world = model.modelMatrix * float4(v.position, 1.0);
    float4 clip  = scene.projViewMatrix * world;
    clip.z = 0.5 * (clip.z + clip.w);   // GL [-1,1] -> Metal [0,1]
    out.position = clip;
    out.worldNormal = normalize((model.normalMatrix * float4(v.normal, 0.0)).xyz) * model.normalSign;
    return out;
}

fragment float4 fragmentShader(Varyings in [[stage_in]])
{
    float3 N = normalize(in.worldNormal);
    float3 L = normalize(float3(0.4, 0.75, 0.5));

    // Quantise diffuse into 4 cel bands.
    float ndl   = max(dot(N, L), 0.0);
    float bands = 4.0;
    float toon  = floor(ndl * bands) / (bands - 1.0);
    toon = clamp(toon, 0.0, 1.0);

    // Warm lit colour, cool shadow colour.
    float3 warm = float3(0.95, 0.55, 0.25);
    float3 cool = float3(0.10, 0.14, 0.30);
    float3 color = mix(cool, warm, toon);

    // Bold dark ink rim from the upward-facing gradient for a cel outline feel.
    float rim = smoothstep(0.35, 0.0, abs(N.y));
    color *= (1.0 - 0.35 * rim);

    return float4(color, 1.0);
}
)MSL";

Entity* createShape(Engine* engine, Material* material, const char* type,
                    const Vector3& position, float scale)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position.getX(), position.getY(), position.getZ());
    entity->setLocalScale(scale, scale, scale);
    if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
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
        if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Custom Shader (Toon)", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    if (!window) { shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { shutdown(); return -1; }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) { shutdown(); return -1; }

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

    auto scene = engine->scene();
    // The toon shader outputs final colour directly; keep tonemap neutral-ish.
    scene->setToneMapping(TONEMAP_NONE);

    auto* camera = new Entity();
    camera->setEngine(engine.get());
    if (auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>())) {
        cameraComponent->camera()->setClearColor(Color(0.12f, 0.13f, 0.16f, 1.0f));
    }
    camera->setLocalPosition(0.0f, 1.0f, 9.0f);
    camera->setLocalEulerAngles(-6.0f, 0.0f, 0.0f);
    engine->root()->addChild(camera);

    // The custom toon material — bypasses the PBR pipeline entirely.
    auto toonMaterial = std::make_shared<ShaderMaterial>(
        graphicsDevice, "toon", "vertexShader", "fragmentShader", kToonShaderSource);

    std::vector<Entity*> shapes;
    shapes.push_back(createShape(engine.get(), toonMaterial.get(), "sphere",   Vector3(-3.2f, 0.0f, 0.0f), 1.7f));
    shapes.push_back(createShape(engine.get(), toonMaterial.get(), "torus",    Vector3( 0.0f, 0.0f, 0.0f), 1.7f));
    shapes.push_back(createShape(engine.get(), toonMaterial.get(), "cylinder", Vector3( 3.2f, 0.0f, 0.0f), 1.5f));
    for (auto* s : shapes) {
        engine->root()->addChild(s);
    }

    spdlog::info("*** VisuTwin Custom Shader (Toon) Example ***");
    spdlog::info("ShaderMaterial with a user-supplied Metal toon shader (4-band cel shading). Esc quits.");

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
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(static_cast<double>(nowCounter - prevCounter) /
                                            static_cast<double>(perfFreq));
        prevCounter = nowCounter;
        time += dt;

        // Rotate the shapes so the cel bands sweep across their surfaces.
        for (size_t i = 0; i < shapes.size(); ++i) {
            const float speed = 20.0f + static_cast<float>(i) * 10.0f;
            shapes[i]->setLocalEulerAngles(time * speed * 0.5f, time * speed, 0.0f);
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();
    spdlog::info("*** VisuTwin Custom Shader (Toon) Example Finished ***");
    return 0;
}
