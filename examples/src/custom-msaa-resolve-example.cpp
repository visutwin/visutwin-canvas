// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream render-targets/custom-msaa-resolve.
//
// Multisampled textures (TextureOptions::samples) a shader reads per sample. Two cameras render
// the same scene into two 4x multisampled RGBA16F render targets: one stores its samples for a
// custom resolve, the other resolves them in hardware into a resolve buffer
// (RenderTargetOptions::resolveBuffer). A compose pass shows the two side by side: on the left
// the hardware resolve (the samples averaged in linear HDR, then tone-mapped c / (1 + c)), on the
// right a custom resolve that tone-maps every sample before averaging, which keeps the thin, very
// bright edges smooth where the hardware average turns them into hard steps. A white line marks
// the split. The scene is a ring of 24 thin emissive rods (intensity 30, in four colours, each
// 0.03 x 3.5) turning at different speeds over a dark backdrop; the targets are rendered at a
// quarter of the window's resolution and shown with no filtering, so the pixels are visible
// blocks.
//
// Keys standing in for upstream's controls panel:
//   Left / Right  split -/+ 0.05 (0 .. 1, 0.5 to begin with)
//   [ / ]         pixel scale -/+ 1 (1 .. 8, 4 to begin with)
//   Space         animation on / off
//
// DEVIATIONS:
// - The compose is a quad pass appended to the frame (RenderPassShaderQuad), where upstream draws
//   a box with a shader material through a compose camera; its shader is Slang.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <algorithm>
#include <array>
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
    constexpr int kRodCount = 24;

    // The two resolves side by side: hardware on the left, custom on the right.
    constexpr const char* kComposeSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct ComposeUniforms
{
    float4 params;  // x: split, y: pixel scale, zw: the back buffer's size
    uint4 target;   // xy: the targets' size, z: samples
};
VT_DECLARE_QUAD_UNIFORMS(ComposeUniforms, u)
VT_DECLARE_QUAD_MS_INPUT0(msColor)
VT_DECLARE_QUAD_TEXEL_INPUT1(hwResolved)

float3 tonemap(float3 c)
{
    return c / (1.0 + c);
}

[shader("vertex")]
QuadVarying composeVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 composeFragment(QuadVarying input) : SV_Target0
{
    // upscale with no filtering - one render target pixel covers the pixel scale in screen pixels
    const float2 pixel = input.uv * u.params.zw;
    const int2 dims = int2(u.target.xy);
    const int2 coord = min(int2(pixel / u.params.y), dims - int2(1, 1));
    const float splitX = u.params.x * float(dims.x) * u.params.y;

    float3 color;
    if (pixel.x < splitX) {
        // hardware resolve: samples were averaged in linear HDR, tonemap the average
        color = tonemap(hwResolved.Load(int3(coord, 0)).rgb);
    } else {
        // custom resolve: tonemap each sample, then average
        const int count = int(u.target.z);
        float3 sum = float3(0.0, 0.0, 0.0);
        for (int s = 0; s < count; s++) {
            sum += tonemap(VT_LOAD_MS(msColor, coord, s).rgb);
        }
        color = sum / float(count);
    }

    // split line
    if (abs(pixel.x - splitX) < 1.5) {
        color = float3(1.0, 1.0, 1.0);
    }
    return float4(pow(color, 1.0 / 2.2), 1.0);
}
)slang";

    struct ComposeUniforms
    {
        float params[4];
        uint32_t target[4];
    };

    class ComposePass final : public RenderPassShaderQuad
    {
    public:
        using RenderPassShaderQuad::RenderPassShaderQuad;

        Texture* msColor = nullptr;
        Texture* hwResolved = nullptr;
        ComposeUniforms uniforms{};

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("custom-msaa-resolve-compose", kComposeSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            setQuadUniforms(uniforms);
            setQuadTextureBinding(0, msColor);
            setQuadTextureBinding(1, hwResolved);
            RenderPassShaderQuad::execute();
        }
    };
}

class CustomMsaaResolveExample final : public ExampleApp
{
public:
    CustomMsaaResolveExample() : ExampleApp({.title = "Custom MSAA Resolve"}) {}

protected:
    bool create() override
    {
        // A ring of thin, very bright emissive rods rotating over a dark backdrop - high contrast HDR
        // edges in motion, the worst case for the hardware box resolve
        const std::array<Color, 4> rodColors{Color(1.0f, 0.25f, 0.05f, 1.0f), Color(0.1f, 0.5f, 1.0f, 1.0f),
            Color(0.9f, 0.9f, 0.4f, 1.0f), Color(0.3f, 1.0f, 0.4f, 1.0f)};
        for (int i = 0; i < kRodCount; ++i) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissive(rodColors[i % rodColors.size()]);
            material->setEmissiveIntensity(30.0f);
            _materials.push_back(material);
            Entity* rod = createPrimitive("box", material.get(), Vector3(0.0f, 0.0f, 0.0f),
                Vector3(0.03f, 3.5f, 0.03f));
            rod->setName("rod" + std::to_string(i));
            rod->setLocalEulerAngles(0.0f, 0.0f, (360.0f / kRodCount) * static_cast<float>(i));
            _rods.push_back(rod);
        }
        auto backdropMaterial = std::make_shared<StandardMaterial>();
        backdropMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        backdropMaterial->setEmissive(Color(0.02f, 0.02f, 0.025f, 1.0f));
        _materials.push_back(backdropMaterial);
        Entity* backdrop = createPrimitive("plane", backdropMaterial.get(), Vector3(0.0f, 0.0f, -2.0f),
            Vector3(30.0f, 1.0f, 30.0f));
        backdrop->setName("backdrop");
        backdrop->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

        // Multisampled render targets, one per resolve technique, both rendering the same scene:
        // msColor's samples are stored and read by the custom resolve, msColorHw's are resolved by
        // the hardware into resolveTex at the end of the render pass
        const auto [width, height] = targetSize();
        _msColor = createTexture("msColor", width, height, 4);
        _msColorHw = createTexture("msColorHw", width, height, 4);
        _resolveTex = createTexture("resolveTex", width, height, 1);
        RenderTargetOptions customOptions;
        customOptions.graphicsDevice = device().get();
        customOptions.name = "customRt";
        customOptions.colorBuffer = _msColor.get();
        customOptions.depth = true;
        _customRt = device()->createRenderTarget(customOptions);
        RenderTargetOptions hwOptions;
        hwOptions.graphicsDevice = device().get();
        hwOptions.name = "hwRt";
        hwOptions.colorBuffer = _msColorHw.get();
        hwOptions.resolveBuffer = _resolveTex.get();
        hwOptions.depth = true;
        _hwRt = device()->createRenderTarget(hwOptions);

        // Two identical cameras rendering the scene into the two targets
        createTargetCamera("camCustom", 0, _customRt);
        createTargetCamera("camHw", 1, _hwRt);

        // The compose pass, a fullscreen shader combining the two techniques side by side
        _compose = std::make_shared<ComposePass>(device());
        _compose->init(nullptr);
        _compose->msColor = _msColor.get();
        _compose->hwResolved = _resolveTex.get();
        engine()->renderer()->addAppendPass(_compose);

        spdlog::info("Keys: Left / Right split, [ / ] pixel scale, Space animation");
        return true;
    }

    void update(const float dt) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            if (keyboard->wasPressed(Key::Left)) {
                _split = std::max(_split - 0.05f, 0.0f);
            } else if (keyboard->wasPressed(Key::Right)) {
                _split = std::min(_split + 0.05f, 1.0f);
            } else if (keyboard->wasPressed(Key::LeftBracket)) {
                _pixelScale = std::max(_pixelScale - 1, 1);
            } else if (keyboard->wasPressed(Key::RightBracket)) {
                _pixelScale = std::min(_pixelScale + 1, 8);
            } else if (keyboard->wasPressed(Key::Space)) {
                _animate = !_animate;
            }
        }

        // Rotate the rods
        if (_animate) {
            _time += dt;
            for (int i = 0; i < kRodCount; ++i) {
                _rods[i]->setLocalEulerAngles(0.0f, 0.0f,
                    (360.0f / kRodCount) * static_cast<float>(i) + _time * (5.0f + static_cast<float>(i % 5) * 3.0f));
            }
        }

        // Keep the targets in step with the window and the pixel scale: resizing a target resizes
        // its multisampled colour buffer and its resolve buffer
        const auto [width, height] = targetSize();
        _customRt->resize(width, height);
        _hwRt->resize(width, height);

        const auto [screenWidth, screenHeight] = device()->size();
        _compose->uniforms = {{_split, static_cast<float>(_pixelScale), static_cast<float>(screenWidth),
            static_cast<float>(screenHeight)}, {static_cast<uint32_t>(width), static_cast<uint32_t>(height),
            _msColor->samples(), 0u}};
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
    // The targets' size: the window's at a fraction of its resolution (the pixel scale)
    std::pair<int, int> targetSize() const
    {
        const auto [width, height] = device()->size();
        return {std::max(1, width / _pixelScale), std::max(1, height / _pixelScale)};
    }

    std::shared_ptr<Texture> createTexture(const char* name, const int width, const int height, const uint32_t samples)
    {
        TextureOptions options;
        options.name = name;
        options.width = static_cast<uint32_t>(width);
        options.height = static_cast<uint32_t>(height);
        options.format = PixelFormat::PIXELFORMAT_RGBA16F;
        options.mipmaps = false;
        options.samples = samples;
        return std::make_shared<Texture>(device().get(), options);
    }

    void createTargetCamera(const char* name, const int priority, const std::shared_ptr<RenderTarget>& target)
    {
        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 5.0f));
        camera->setName(name);
        auto* component = camera->findComponent<CameraComponent>();
        component->setPriority(priority);
        component->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
        component->camera()->setRenderTarget(target);
    }

    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _rods;
    std::shared_ptr<Texture> _msColor;
    std::shared_ptr<Texture> _msColorHw;
    std::shared_ptr<Texture> _resolveTex;
    std::shared_ptr<RenderTarget> _customRt;
    std::shared_ptr<RenderTarget> _hwRt;
    std::shared_ptr<ComposePass> _compose;
    float _split = 0.5f;
    int _pixelScale = 4;
    bool _animate = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(CustomMsaaResolveExample)
