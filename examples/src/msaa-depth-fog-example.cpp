// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream render-targets/msaa-depth-fog.
//
// Fog computed per sample from a multisampled depth texture, against fog from a resolved depth.
// A camera renders rows of thin emissive pillars receding into the distance into a 4x
// multisampled RGBA16F colour texture and a multisampled depth texture; at the end of the pass
// the depth is also resolved into a single-sampled R32F texture (RenderTargetOptions::
// depthResolveBuffer) by the selected mode. A compose pass shows two fog techniques side by side:
// on the left one fog value from the resolved depth for the whole pixel, over the averaged colour,
// whose fog boundary cannot match the antialiased colour at the silhouettes whichever resolve mode
// made the depth; on the right every sample fogged at its own depth and then averaged, which
// matches exactly. Fog is 1 - exp(-density x linear depth) toward (0.48, 0.52, 0.6). A white line
// marks the split. 60 pillars (0.12 x 4, emissive 2.5, four colours) stand over a dark ground; the
// camera (near 0.5, far 60) sways about (0, 0.5, 3). The target is rendered at a fifth of the
// window's resolution and shown with no filtering.
//
// Keys standing in for upstream's controls panel:
//   Left / Right  split -/+ 0.05 (0 .. 1, 0.5 to begin with)
//   M             depth resolve mode: min, max, sample 0 (min to begin with)
//   Down / Up     fog density -/+ 0.01 (0.01 .. 0.4, 0.1 to begin with)
//   [ / ]         pixel scale -/+ 1 (1 .. 8, 5 to begin with)
//   Space         animation on / off
//
// DEVIATIONS:
// - The compose is a quad pass appended to the frame (RenderPassShaderQuad), where upstream draws
//   a box with a shader material through a compose camera; its shader is Slang.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float kNear = 0.5f;
    constexpr float kFar = 60.0f;

    // The two fog techniques side by side: from the resolved depth on the left, per sample on the
    // right.
    constexpr const char* kComposeSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct ComposeUniforms
{
    float4 params;  // x: split, y: pixel scale, zw: the back buffer's size
    float4 fog;     // x: density, y: near, z: far
    uint4 target;   // xy: the target's size, z: samples
};
VT_DECLARE_QUAD_UNIFORMS(ComposeUniforms, u)
VT_DECLARE_QUAD_MS_INPUT0(msColor)
VT_DECLARE_QUAD_DEPTH_MS_INPUT1(msDepth)
VT_DECLARE_QUAD_TEXEL_INPUT2(resolvedDepth)

float linearizeDepth(float d)
{
    const float near = u.fog.y;
    const float far = u.fog.z;
    return near * far / (far - d * (far - near));
}

float fogFactor(float linearDepth)
{
    return 1.0 - exp(-u.fog.x * linearDepth);
}

[shader("vertex")]
QuadVarying composeVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 composeFragment(QuadVarying input) : SV_Target0
{
    const float3 fogColor = float3(0.48, 0.52, 0.6);
    const float2 pixel = input.uv * u.params.zw;
    const int2 dims = int2(u.target.xy);
    const int2 coord = min(int2(pixel / u.params.y), dims - int2(1, 1));
    const float splitX = u.params.x * float(dims.x) * u.params.y;
    const int count = int(u.target.z);

    float3 color;
    if (pixel.x < splitX) {
        // fog from the resolved depth - one fog value for the whole pixel, applied to the averaged
        // color
        float3 avg = float3(0.0, 0.0, 0.0);
        for (int s = 0; s < count; s++) {
            avg += VT_LOAD_MS(msColor, coord, s).rgb;
        }
        avg /= float(count);
        const float lin = linearizeDepth(resolvedDepth.Load(int3(coord, 0)).r);
        color = lerp(avg, fogColor, fogFactor(lin));
    } else {
        // per-sample fog - each sample is fogged at its own depth, then averaged
        float3 sum = float3(0.0, 0.0, 0.0);
        for (int s = 0; s < count; s++) {
            const float3 c = VT_LOAD_MS(msColor, coord, s).rgb;
            const float lin = linearizeDepth(VT_LOAD_MS(msDepth, coord, s));
            sum += lerp(c, fogColor, fogFactor(lin));
        }
        color = sum / float(count);
    }

    // split line
    if (abs(pixel.x - splitX) < 1.5) {
        color = float3(1.0, 1.0, 1.0);
    }
    return float4(pow(max(color, float3(0.0, 0.0, 0.0)), 1.0 / 2.2), 1.0);
}
)slang";

    struct ComposeUniforms
    {
        float params[4];
        float fog[4];
        uint32_t target[4];
    };

    class ComposePass final : public RenderPassShaderQuad
    {
    public:
        using RenderPassShaderQuad::RenderPassShaderQuad;

        Texture* msColor = nullptr;
        Texture* msDepth = nullptr;
        Texture* resolvedDepth = nullptr;
        ComposeUniforms uniforms{};

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("msaa-depth-fog-compose", kComposeSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            setQuadUniforms(uniforms);
            setQuadTextureBinding(0, msColor);
            setQuadTextureBinding(1, msDepth);
            setQuadTextureBinding(2, resolvedDepth);
            RenderPassShaderQuad::execute();
        }
    };

    constexpr std::array<DepthResolveMode, 3> kModes{DepthResolveMode::DEPTHRESOLVE_MIN,
        DepthResolveMode::DEPTHRESOLVE_MAX, DepthResolveMode::DEPTHRESOLVE_SAMPLE0};
    constexpr std::array<const char*, 3> kModeNames{"min", "max", "sample 0"};
}

class MsaaDepthFogExample final : public ExampleApp
{
public:
    MsaaDepthFogExample() : ExampleApp({.title = "MSAA Depth Fog"}) {}

protected:
    bool create() override
    {
        // Rows of thin emissive pillars receding into the distance - high contrast depth
        // discontinuities at their silhouettes, the worst case for fog from resolved depth
        const std::array<Color, 4> pillarColors{Color(1.0f, 0.5f, 0.2f, 1.0f), Color(0.3f, 0.7f, 1.0f, 1.0f),
            Color(0.9f, 0.9f, 0.6f, 1.0f), Color(0.5f, 1.0f, 0.6f, 1.0f)};
        for (int i = 0; i < 60; ++i) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissive(pillarColors[i % pillarColors.size()]);
            material->setEmissiveIntensity(2.5f);
            _materials.push_back(material);
            const float z = -3.0f - static_cast<float>(i % 15) * 1.8f;
            // JavaScript's % keeps the dividend's sign, as fmod does
            const double hash = std::fmod(std::sin(static_cast<double>(i) * 12.9898) * 43758.5453, 1.0);
            const float x = static_cast<float>(hash) * 8.0f - 4.0f;
            Entity* pillar = createPrimitive("box", material.get(), Vector3(x, 0.0f, z), Vector3(0.12f, 4.0f, 0.12f));
            pillar->setName("pillar" + std::to_string(i));
        }
        auto groundMaterial = std::make_shared<StandardMaterial>();
        groundMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        groundMaterial->setEmissive(Color(0.04f, 0.04f, 0.05f, 1.0f));
        _materials.push_back(groundMaterial);
        Entity* ground = createPrimitive("plane", groundMaterial.get(), Vector3(0.0f, -2.0f, -15.0f),
            Vector3(60.0f, 1.0f, 60.0f));
        ground->setName("ground");

        // The render target: multisampled colour and depth textures, with the depth also resolved
        // into a single-sampled R32F texture at the end of the pass, by the selected mode
        const auto [width, height] = targetSize();
        _msColor = createTexture("msColor", width, height, PixelFormat::PIXELFORMAT_RGBA16F, 4);
        _msDepth = createTexture("msDepth", width, height, PixelFormat::PIXELFORMAT_DEPTH, 4);
        _resolvedDepth = createTexture("resolvedDepth", width, height, PixelFormat::PIXELFORMAT_R32F, 1);
        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.name = "sceneRt";
        options.colorBuffer = _msColor.get();
        options.depthBuffer = _msDepth.get();
        options.depthResolveBuffer = _resolvedDepth.get();
        options.depthResolveMode = kModes[_mode];
        _sceneRt = device()->createRenderTarget(options);

        // The scene camera rendering into the multisampled target
        _camera = createCamera(Vector3(0.0f, 0.5f, 3.0f));
        _camera->setName("camScene");
        auto* component = _camera->findComponent<CameraComponent>();
        component->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        component->camera()->setNearClip(kNear);
        component->camera()->setFarClip(kFar);
        component->camera()->setRenderTarget(_sceneRt);

        // The compose pass, a fullscreen shader combining the two fog techniques side by side
        _compose = std::make_shared<ComposePass>(device());
        _compose->init(nullptr);
        _compose->msColor = _msColor.get();
        _compose->msDepth = _msDepth.get();
        _compose->resolvedDepth = _resolvedDepth.get();
        engine()->renderer()->addAppendPass(_compose);

        spdlog::info("Keys: Left / Right split, M resolve mode, Down / Up density, [ / ] pixel scale, Space animation");
        return true;
    }

    void update(const float dt) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            if (keyboard->wasPressed(Key::Left)) {
                _split = std::max(_split - 0.05f, 0.0f);
            } else if (keyboard->wasPressed(Key::Right)) {
                _split = std::min(_split + 0.05f, 1.0f);
            } else if (keyboard->wasPressed(Key::M)) {
                _mode = (_mode + 1) % kModes.size();
                spdlog::info("Depth resolve mode: {}", kModeNames[_mode]);
            } else if (keyboard->wasPressed(Key::Down)) {
                _density = std::max(_density - 0.01f, 0.01f);
            } else if (keyboard->wasPressed(Key::Up)) {
                _density = std::min(_density + 0.01f, 0.4f);
            } else if (keyboard->wasPressed(Key::LeftBracket)) {
                _pixelScale = std::max(_pixelScale - 1, 1);
            } else if (keyboard->wasPressed(Key::RightBracket)) {
                _pixelScale = std::min(_pixelScale + 1, 8);
            } else if (keyboard->wasPressed(Key::Space)) {
                _animate = !_animate;
            }
        }

        // Sway the camera
        if (_animate) {
            _time += dt;
            _camera->setLocalPosition(std::sin(_time * 0.3f) * 0.8f, 0.5f + std::sin(_time * 0.13f) * 0.3f, 3.0f);
        }

        // Keep the target in step with the window and the pixel scale: resizing it resizes the
        // multisampled colour and depth and the depth resolve buffer
        const auto [width, height] = targetSize();
        _sceneRt->resize(width, height);
        // The depth resolve mode can be changed at any time
        _sceneRt->setDepthResolveMode(kModes[_mode]);

        const auto [screenWidth, screenHeight] = device()->size();
        _compose->uniforms = {{_split, static_cast<float>(_pixelScale), static_cast<float>(screenWidth),
            static_cast<float>(screenHeight)}, {_density, kNear, kFar, 0.0f},
            {static_cast<uint32_t>(width), static_cast<uint32_t>(height), _msColor->samples(), 0u}};
    }

    void destroy() override
    {
        // The pass is registered with the renderer, so it goes while the engine is alive.
        if (engine() && engine()->renderer() && _compose) {
            engine()->renderer()->removeAppendPass(_compose);
        }
        _compose.reset();
    }

private:
    // The target's size: the window's at a fraction of its resolution (the pixel scale)
    std::pair<int, int> targetSize() const
    {
        const auto [width, height] = device()->size();
        return {std::max(1, width / _pixelScale), std::max(1, height / _pixelScale)};
    }

    std::shared_ptr<Texture> createTexture(const char* name, const int width, const int height,
        const PixelFormat format, const uint32_t samples)
    {
        TextureOptions options;
        options.name = name;
        options.width = static_cast<uint32_t>(width);
        options.height = static_cast<uint32_t>(height);
        options.format = format;
        options.mipmaps = false;
        options.samples = samples;
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        return std::make_shared<Texture>(device().get(), options);
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::shared_ptr<Texture> _msColor;
    std::shared_ptr<Texture> _msDepth;
    std::shared_ptr<Texture> _resolvedDepth;
    std::shared_ptr<RenderTarget> _sceneRt;
    std::shared_ptr<ComposePass> _compose;
    Entity* _camera = nullptr;
    float _split = 0.5f;
    float _density = 0.1f;
    size_t _mode = 0;
    int _pixelScale = 5;
    bool _animate = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(MsaaDepthFogExample)
