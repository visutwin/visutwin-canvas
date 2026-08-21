// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Custom shader material example — port of upstream shaders/shader-toon.
// A ShaderMaterial carrying a user-supplied toon shader (quantised N·L into 6 bands
// over a single warm-grey ramp) replaces the materials of every mesh instance in the
// loaded statue model, which rotates at 60°/s.
//
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#endif

#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <vector>

#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include "framework/engine.h"
#include "log.h"
#include "framework/appOptions.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
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

const std::string rootPath = ASSET_DIR;

const auto statueAsset = std::make_unique<Asset>(
    "statue",
    AssetType::CONTAINER,
    rootPath + "/models/statue.glb"
);

// Self-contained toon shader. It declares the same vertex-input attributes and
// per-draw uniform buffers the forward pass binds (SceneData @1, ModelData @2), plus
// its own custom block at @3 in place of MaterialData — see ToonMaterial below.
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

// Replaces MaterialData at buffer(3) — the port's equivalent of upstream's
// material.setParameter('uLightPos', ...).
struct ToonData { float4 lightPos; };

struct Varyings {
    float4 position [[position]];
    float  vertOutTexCoord;
    float2 texCoord;
};

vertex Varyings vertexShader(VertexData v [[stage_in]],
                             constant SceneData &scene [[buffer(1)]],
                             constant ModelData &model [[buffer(2)]],
                             constant ToonData  &toon  [[buffer(3)]])
{
    Varyings out;
    float4 world = model.modelMatrix * float4(v.position, 1.0);
    float3 worldNormal = normalize((model.normalMatrix * float4(v.normal, 0.0)).xyz) * model.normalSign;

    // Vector to the light source.
    float3 lightDir = normalize(toon.lightPos.xyz - world.xyz);

    // Dot product gives us diffuse intensity, used as the 1D colour ramp
    // coordinate in the fragment shader.
    out.vertOutTexCoord = max(0.0, dot(worldNormal, lightDir));
    out.texCoord = v.uv0;

    float4 clip = scene.projViewMatrix * world;
    clip.z = 0.5 * (clip.z + clip.w);   // GL [-1,1] -> Metal [0,1]
    out.position = clip;
    return out;
}

fragment float4 fragmentShader(Varyings in [[stage_in]])
{
    float v = in.vertOutTexCoord;
    v = float(int(v * 6.0)) / 6.0;
    float3 linearColor = float3(0.218, 0.190, 0.156) * v;
    // gammaCorrectOutput: nothing else in the pipeline encodes a custom shader's output.
    return float4(pow(linearColor + 0.0000001, float3(1.0 / 2.2)), 1.0);
}
)MSL";

// The same toon shader in GLSL, for the Vulkan backend.
//
// Differences from the MSL above, all forced by the backend rather than by choice:
//  - transforms arrive in a 128-byte vertex push constant (viewProjection, model),
//    not in buffer(1)/buffer(2) uniform blocks;
//  - there is no normalMatrix/normalSign, so the normal is transformed by mat3(model),
//    exactly as engine/shaders/vulkan/forward.vert does;
//  - the custom uniform block is set 0 / binding 0 (the per-draw material UBO, bound
//    to both stages) rather than buffer(3);
//  - the clip.z remap is MANDATORY. Engine projections are GL-style (NDC z in [-1,1])
//    while Vulkan clips to [0,w], so forward.vert (and particle.vert / gsplat.vert)
//    all apply `z = 0.5 * (z + w)`. A custom shader that skips it stores roughly half
//    the depth every other draw stores, and then wrongly wins depth tests against all
//    standard-material geometry — verified 2026-08-21 with a box that the statue
//    occluded on Vulkan but correctly intersected on Metal.
static const char* kToonShaderSourceGlsl = R"GLSL(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform ToonData {
    vec4 uLightPos;
} toon;

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec3 vertexNormal;
layout(location = 0) out float vertOutTexCoord;
void main() {
    vec4 world = pc.model * vec4(vertexPosition, 1.0);
    vec3 worldNormal = normalize(mat3(pc.model) * vertexNormal);
    vec3 lightDir = normalize(toon.uLightPos.xyz - world.xyz);
    vertOutTexCoord = max(0.0, dot(worldNormal, lightDir));
    vec4 clip = pc.viewProjection * world;
    clip.z = 0.5 * (clip.z + clip.w);   // GL [-1,1] -> Vulkan [0,1], as forward.vert does
    gl_Position = clip;
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) in float vertOutTexCoord;
layout(location = 0) out vec4 fragColor;
void main() {
    float v = vertOutTexCoord;
    v = float(int(v * 6.0)) / 6.0;
    vec3 linearColor = vec3(0.218, 0.190, 0.156) * v;
    fragColor = vec4(pow(linearColor + 0.0000001, vec3(1.0 / 2.2)), 1.0);
}
#endif
)GLSL";

// ShaderMaterial carrying one custom uniform. The engine has no named-parameter path
// for user shaders (upstream's setParameter('uLightPos', ...)), so the material
// supplies its whole uniform block instead: customUniformData() replaces MaterialData
// at buffer(3) / set 0 with these bytes.
class ToonMaterial final : public ShaderMaterial
{
public:
    using ShaderMaterial::ShaderMaterial;

    void setLightPosition(const Vector3& position)
    {
        _data.lightPos[0] = position.getX();
        _data.lightPos[1] = position.getY();
        _data.lightPos[2] = position.getZ();
        _data.lightPos[3] = 1.0f;
    }

    const void* customUniformData(size_t& outSize) const override
    {
        outSize = sizeof(_data);
        return &_data;
    }

private:
    struct alignas(16) ToonData
    {
        float lightPos[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    } _data;
};

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

#ifdef VISUTWIN_HAS_METAL
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
#endif
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "Custom Shader (Toon)", WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
#ifdef VISUTWIN_HAS_VULKAN
        | SDL_WINDOW_VULKAN
#endif
    );
    if (!window) { shutdown(); return -1; }
    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) { shutdown(); return -1; }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    void* swapchain = nullptr;
#ifdef VISUTWIN_HAS_METAL
    swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) { shutdown(); return -1; }
#endif

    GraphicsDeviceOptions deviceOptions;
#ifdef VISUTWIN_HAS_VULKAN
    deviceOptions.backend = Backend::Vulkan;
#endif
    deviceOptions.swapChain = swapchain;
    deviceOptions.window = window;
    auto device = createGraphicsDevice(deviceOptions);
    if (!device) { shutdown(); return -1; }

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
    scene->setAmbientLight(0.2f, 0.2f, 0.2f);

    // Camera. Upstream translates it without rotating, so it looks straight down -Z.
    auto* camera = new Entity();
    camera->setEngine(engine.get());
    if (auto* cameraComponent = static_cast<CameraComponent*>(camera->addComponent<CameraComponent>())) {
        cameraComponent->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
    }
    const Vector3 cameraPosition(0.0f, 7.0f, 24.0f);
    camera->setLocalPosition(cameraPosition);
    engine->root()->addChild(camera);

    // Omni light. The toon shader does its own lighting, so this entity only supplies
    // the light position the material passes to the shader.
    auto* light = new Entity();
    light->setEngine(engine.get());
    if (auto* lc = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
        lc->setType(LightType::LIGHTTYPE_OMNI);
        lc->setColor(Color(1.0f, 1.0f, 1.0f));
        lc->setRange(10.0f);
    }
    const Vector3 lightPosition(0.0f, 1.0f, 0.0f);
    light->setLocalPosition(lightPosition);
    engine->root()->addChild(light);

    // Custom toon material — bypasses the PBR pipeline entirely.
    auto toonMaterial = std::make_shared<ToonMaterial>(
        graphicsDevice, "toon", "vertexShader", "fragmentShader",
        ShaderSourceSet{.msl = kToonShaderSource, .glsl = kToonShaderSourceGlsl});

    // DEVIATION: upstream's shader compares a WORLD-space normal (matrix_normal is the
    // world normal matrix, despite the "eye coordinates" comment) against a light
    // direction built from a VIEW-space vertex position. With its unrotated camera the
    // view matrix is a pure translation, so that mix is exactly a world-space light at
    // lightPosition + cameraPosition — which is what this port feeds the shader, keeping
    // the lighting identical while the shader stays consistently world-space.
    toonMaterial->setLightPosition(lightPosition + cameraPosition);

    const auto statueResource = statueAsset->resource();
    if (!statueResource) {
        spdlog::error("Failed to load models/statue.glb");
        shutdown();
        return -1;
    }
    auto* statue = std::get<ContainerResource*>(*statueResource)->instantiateRenderEntity();
    engine->root()->addChild(statue);

    // Set the new material on every mesh in the model.
    int meshInstanceCount = 0;
    for (auto* render : statue->findComponents<RenderComponent>()) {
        for (auto* meshInstance : render->meshInstances()) {
            meshInstance->setMaterial(toonMaterial.get());
            ++meshInstanceCount;
        }
    }

    spdlog::info("*** Custom Shader (Toon) Example ***");
    spdlog::info("ShaderMaterial toon shader applied to {} mesh instances. Esc quits.",
                 meshInstanceCount);

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float angle = 0.0f;

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

        // Rotate the statue.
        angle += 60.0f * dt;
        statue->setLocalEulerAngles(0.0f, angle, 0.0f);

        engine->update(dt);
        engine->render();
    }

    shutdown();
    spdlog::info("*** Custom Shader (Toon) Example Finished ***");
    return 0;
}
