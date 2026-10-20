// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 20.10.2026
//
// Port of upstream render-targets/render-pass-shader-quad.
//
// Paints glowing light trails into a 1024 x 1024 texture with two shader-quad passes attached to
// no camera. Each frame a fade pass darkens what the texture holds (a reverse-subtract blend:
// canvas * (1 - a) - rgb), then a brush pass adds a soft dot along the path of each brush, every
// dot a quad drawn into a small viewport around it, optionally inside a scissor rectangle that
// keeps a border clear. Five brushes follow Lissajous curves in hues that drift over time. The
// texture is cleared only on the first frame and when C is pressed, and is the emissive map of a
// dark, polished, partly metallic cube turning under the studio env atlas (mip 2), seen with
// neutral tone mapping. The texture is shown in the bottom right corner.
//
// Keys stand in for upstream's controls panel:
//   S / A  brush count +/- 1 (1 .. 8)
//   ] / [  brush size +/- 4 (8 .. 64)
//   . / ,  speed +/- 0.5 (0.1 .. 4)
//   B      blend: additive, normal, lighten
//   T / G  trail length +/- 0.5 (0.1 .. 6)
//   O      border on / off
//   C      clear the texture
//
// DEVIATIONS:
// - the passes render at the START of the frame graph (Renderer::addPrependPass) where upstream
//   renders them from the "prerender" event, which fires here before a frame has begun.
// - the texture is RGBA16F on both backends (both render to half float); upstream falls back to
//   an sRGB 8-bit format, which this engine does not have.
// - upstream's TextureRenderer is a RenderPassDownsample appended to the frame, its viewport the
//   preview rectangle; the orbit-camera script is CameraControls in orbit mode with the same
//   distance range; anisotropy is the engine's material sampler's.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/materials/standardMaterial.h"
#include "scene/renderer/forwardRenderer.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kCanvasSize = 1024;
    constexpr int kMaxBrushes = 8;

    // The light the fade removes: with the pass's reverse subtract blending the canvas becomes
    // canvas * (1 - fade.a) - fade.rgb. The alpha fades the trails smoothly, the colour removes a
    // small constant amount that fades them out fully.
    const char* const kFadeSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct FadeUniforms
{
    float4 fade;
};
VT_DECLARE_QUAD_UNIFORMS(FadeUniforms, u)

[shader("vertex")]
QuadVarying fadeVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 fadeFragment(QuadVarying input) : SV_Target0
{
    return u.fade;
}
)slang";

    // A soft dot with a white-hot core, filling the viewport the quad is drawn into. The colour is
    // premultiplied by the alpha, as the pass's blend states expect.
    const char* const kBrushSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct BrushUniforms
{
    float4 color;   // rgb: the brush colour
    float4 params;  // x: the brightness of a single dot
};
VT_DECLARE_QUAD_UNIFORMS(BrushUniforms, u)

[shader("vertex")]
QuadVarying brushVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 brushFragment(QuadVarying input) : SV_Target0
{
    // the distance from the centre of the dot, 1 at the edge of the viewport
    const float dist = length(input.uv * 2.0 - 1.0);

    float glow = 1.0 - smoothstep(0.0, 1.0, dist);
    glow *= glow;
    const float core = 1.0 - smoothstep(0.0, 0.3, dist);

    const float intensity = u.params.x;
    const float3 color = (u.color.rgb * glow + float3(0.7 * core)) * intensity;
    return float4(color, glow * intensity);
}
)slang";

    struct FadeUniforms
    {
        float fade[4];
    };

    struct BrushUniforms
    {
        float color[4];
        float params[4];
    };

    // Blending on, the same equation and factors for colour and alpha.
    std::shared_ptr<BlendState> makeBlend(const int op, const int src, const int dst)
    {
        auto state = std::make_shared<BlendState>();
        state->setEnabled(true);
        state->setColorOp(op);
        state->setColorSrcFactor(src);
        state->setColorDstFactor(dst);
        state->setAlphaOp(op);
        state->setAlphaSrcFactor(src);
        state->setAlphaDstFactor(dst);
        return state;
    }

    class RenderPassFade final : public RenderPassShaderQuad
    {
    public:
        explicit RenderPassFade(const std::shared_ptr<GraphicsDevice>& device) : RenderPassShaderQuad(device)
        {
            setBlendState(makeBlend(BLENDEQUATION_REVERSE_SUBTRACT, BLENDMODE_ONE, BLENDMODE_ONE_MINUS_SRC_ALPHA));
        }

        std::array<float, 4> fade{};

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("render-pass-shader-quad-fade", kFadeSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            setQuadUniforms(FadeUniforms{{fade[0], fade[1], fade[2], fade[3]}});
            RenderPassShaderQuad::execute();
        }
    };

    // Every dot of the frame, each a quad drawn into the viewport around it, in one device pass.
    class RenderPassBrush final : public RenderPassShaderQuad
    {
    public:
        explicit RenderPassBrush(const std::shared_ptr<GraphicsDevice>& device) : RenderPassShaderQuad(device) {}

        struct Stamp
        {
            Vector4 rect;
            std::array<float, 3> color{};
        };
        std::vector<Stamp> stamps;
        float intensity = 1.0f;

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("render-pass-shader-quad-brush", kBrushSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            for (const auto& stamp : stamps) {
                setQuadUniforms(BrushUniforms{{stamp.color[0], stamp.color[1], stamp.color[2], 1.0f},
                    {intensity, 0.0f, 0.0f, 0.0f}});
                setViewport(stamp.rect);
                RenderPassShaderQuad::execute();
            }
            stamps.clear();
        }
    };

    struct Brush
    {
        float frequencyX = 0.0f;
        float frequencyY = 0.0f;
        float phaseX = 0.0f;
        float phaseY = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        bool started = false;
    };

    // A fully saturated colour of the hue, 0 .. 1.
    std::array<float, 3> hueToColor(const float hue)
    {
        const float h = hue * 6.0f;
        return {std::clamp(std::abs(h - 3.0f) - 1.0f, 0.0f, 1.0f), std::clamp(2.0f - std::abs(h - 2.0f), 0.0f, 1.0f),
            std::clamp(2.0f - std::abs(h - 4.0f), 0.0f, 1.0f)};
    }

    enum class BlendKind { Add, Normal, Lighten };
    constexpr std::array<const char*, 3> kBlendNames{"additive", "normal", "lighten"};
}

class RenderPassShaderQuadExample final : public ExampleApp
{
public:
    RenderPassShaderQuadExample() : ExampleApp({.title = "Render Pass Shader Quad"}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("studio-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* envAtlas = _envAtlas->resourceAs<Texture>();
        if (!envAtlas) {
            spdlog::error("render-pass-shader-quad needs the studio env atlas");
            return false;
        }
        scene()->setEnvAtlas(envAtlas);
        scene()->setSkyboxMip(2);

        // ------ The canvas the trails are painted into ------
        // Half float keeps the small steps of the slowly fading trails, and light brighter than 1.
        TextureOptions options;
        options.name = "TrailsCanvas";
        options.width = kCanvasSize;
        options.height = kCanvasSize;
        options.format = PixelFormat::PIXELFORMAT_RGBA16F;
        options.mipmaps = true;
        options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        _canvasTexture = std::make_shared<Texture>(device().get(), options);
        _canvasTexture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _canvasTexture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device().get();
        targetOptions.colorBuffer = _canvasTexture.get();
        targetOptions.depth = false;
        targetOptions.name = "TrailsCanvas";
        _canvasTarget = device()->createRenderTarget(targetOptions);

        // ------ The passes, at the start of every frame: the fade, then the brushes ------
        // The fade loads the canvas, and its mipmaps are only needed after the brushes.
        _fadePass = std::make_shared<RenderPassFade>(device());
        _fadePass->init(_canvasTarget);
        _fadePass->colorOps()->genMipmaps = false;
        _brushPass = std::make_shared<RenderPassBrush>(device());
        _brushPass->init(_canvasTarget);
        engine()->renderer()->addPrependPass(_fadePass);
        engine()->renderer()->addPrependPass(_brushPass);

        // The blend states the brushes paint with, for the brush shader's premultiplied colours.
        _blendStates = {
            makeBlend(BLENDEQUATION_ADD, BLENDMODE_ONE, BLENDMODE_ONE),                  // adds the light
            makeBlend(BLENDEQUATION_ADD, BLENDMODE_ONE, BLENDMODE_ONE_MINUS_SRC_ALPHA),  // paints over
            makeBlend(BLENDEQUATION_MAX, BLENDMODE_ONE, BLENDMODE_ONE),                  // the brighter
        };

        // ------ The scene ------
        // A dark, partly metallic, polished cube reflecting the studio, the canvas its emissive map.
        _material = std::make_shared<StandardMaterial>();
        _material->setDiffuse(Color(0.3f, 0.31f, 0.34f, 1.0f));
        _material->setUseMetalness(true);
        _material->setMetalness(0.6f);
        _material->setGloss(0.92f);
        _material->setEmissiveMap(_canvasTexture.get());
        _material->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _cube = createPrimitive("box", _material.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(1.6f, 1.6f, 1.6f));
        _cube->setName("Cube");

        // Neutral tone mapping keeps the saturated colours of the trails, which ACES would shift
        // in hue and bleach where they are bright.
        Entity* camera = createCamera(Vector3(0.85f, 1.25f, 5.08f));
        if (auto* component = camera->findComponent<CameraComponent>()) {
            component->setToneMapping(TONEMAP_NEUTRAL);
            component->camera()->setFarClip(100.0f);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setZoomRange(Vector2(2.5f, 12.0f));
            controls->storeResetState();
        }

        // ------ The brushes, each on its own Lissajous curve ------
        for (int i = 0; i < kMaxBrushes; ++i) {
            Brush& brush = _brushes[static_cast<size_t>(i)];
            brush.frequencyX = 0.31f + 0.13f * static_cast<float>(i);
            brush.frequencyY = 0.47f + 0.07f * static_cast<float>((i * 5) % 8);
            brush.phaseX = static_cast<float>(i) * 1.7f;
            brush.phaseY = static_cast<float>(i) * 2.9f;
        }

        // ------ The preview in the bottom right corner ------
        _preview = std::make_shared<RenderPassDownsample>(device(), _canvasTexture.get());
        _preview->init(nullptr);
        _preview->setRequiresCubemaps(false);
        engine()->renderer()->addAppendPass(_preview);

        spdlog::info("Keys: S/A count, ] [ size, . , speed, B blend, T/G trail, O border, C clear");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_brushPass) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_S: _count = std::min(kMaxBrushes, _count + 1); break;
        case SDLK_A: _count = std::max(1, _count - 1); break;
        case SDLK_RIGHTBRACKET: _size = std::min(64, _size + 4); break;
        case SDLK_LEFTBRACKET: _size = std::max(8, _size - 4); break;
        case SDLK_PERIOD: _speed = std::min(4.0f, _speed + 0.5f); break;
        case SDLK_COMMA: _speed = std::max(0.1f, _speed - 0.5f); break;
        case SDLK_B: _blend = static_cast<BlendKind>((static_cast<int>(_blend) + 1) % 3); break;
        case SDLK_T: _trail = std::min(6.0f, _trail + 0.5f); break;
        case SDLK_G: _trail = std::max(0.1f, _trail - 0.5f); break;
        case SDLK_O: _border = !_border; break;
        case SDLK_C: _clearPending = true; break;
        default: return false;
        }
        spdlog::info("brushes {}, size {}, speed {}, blend {}, trail {}, border {}", _count, _size, _speed,
            kBlendNames[static_cast<size_t>(_blend)], _trail, _border ? "on" : "off");
        return true;
    }

    void update(const float dt) override
    {
        if (!_fadePass) {
            return;
        }
        _time += dt * _speed;

        // The fade clears the canvas in the frame it is asked to, and loads it from then on.
        if (_clearPending) {
            const Color black(0.0f, 0.0f, 0.0f, 1.0f);
            _fadePass->setClearColor(&black);
            _clearPending = false;
        } else {
            _fadePass->setClearColor(nullptr);
        }

        // The same fade at any frame rate: a fraction of the light, and a small constant amount
        // that removes the last of the trails.
        _fadePass->fade[3] = 1.0f - std::exp(-dt / _trail);
        _fadePass->fade[0] = _fadePass->fade[1] = _fadePass->fade[2] = 0.02f * dt / _trail;

        // The brushes. Overlapping dots add up when adding the light, so each is dimmer.
        addStamps();
        _brushPass->setBlendState(_blendStates[static_cast<size_t>(_blend)]);
        _brushPass->intensity = _blend == BlendKind::Add ? 0.25f : 1.0f;
        if (_border) {
            const float border = kCanvasSize * 0.1f;
            _brushPass->setScissor(Vector4(border, border, kCanvasSize - 2.0f * border, kCanvasSize - 2.0f * border));
        } else {
            _brushPass->clearScissor();
        }

        _cube->rotate(4.0f * dt, 9.0f * dt, 0.0f);

        // A square preview at the bottom right corner.
        const auto [width, height] = device()->size();
        const float side = std::floor(0.25f * static_cast<float>(std::min(width, height)));
        const float margin = std::floor(0.08f * side);
        _preview->setViewport(Vector4(static_cast<float>(width) - side - margin,
            static_cast<float>(height) - side - margin, side, side));
        _preview->setScissor(_preview->viewport());
    }

    void destroy() override
    {
        if (engine() && engine()->renderer()) {
            engine()->renderer()->removePrependPass(_fadePass);
            engine()->renderer()->removePrependPass(_brushPass);
            engine()->renderer()->removeAppendPass(_preview);
        }
        _fadePass.reset();
        _brushPass.reset();
        _preview.reset();
    }

private:
    // The dots of each brush along its path since the previous frame, so the trails stay
    // continuous at any speed. Positions are canvas pixels, and a viewport's y runs down the
    // texture as v does, so a dot lands at the same texture coordinate as upstream's.
    void addStamps()
    {
        const float radius = static_cast<float>(_size);
        const float spacing = radius * 0.25f;
        const float amplitude = (kCanvasSize - 2.0f * radius) * 0.5f * 0.9f;
        const float half = kCanvasSize * 0.5f;

        for (int i = 0; i < _count; ++i) {
            Brush& brush = _brushes[static_cast<size_t>(i)];
            const float x = half + amplitude * std::sin(brush.frequencyX * _time + brush.phaseX);
            const float y = half + amplitude * std::sin(brush.frequencyY * _time + brush.phaseY);

            // a brush starting this frame paints a single dot
            if (!brush.started) {
                brush.started = true;
                brush.x = x;
                brush.y = y;
            }

            const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(x - brush.x, y - brush.y) / spacing)));
            for (int s = 1; s <= steps; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(steps);
                const float cx = brush.x + (x - brush.x) * t;
                const float cy = brush.y + (y - brush.y) * t;

                // the viewport is kept within the canvas
                RenderPassBrush::Stamp stamp;
                stamp.rect = Vector4(std::clamp(cx - radius, 0.0f, kCanvasSize - 2.0f * radius),
                    std::clamp(cy - radius, 0.0f, kCanvasSize - 2.0f * radius), 2.0f * radius, 2.0f * radius);
                stamp.color = hueToColor(std::fmod(static_cast<float>(i) / static_cast<float>(_count) + _time * 0.05f,
                    1.0f));
                _brushPass->stamps.push_back(stamp);
            }

            brush.x = x;
            brush.y = y;
        }

        // the brushes not in use start again from where they are when used
        for (int i = _count; i < kMaxBrushes; ++i) {
            _brushes[static_cast<size_t>(i)].started = false;
        }
    }

    std::unique_ptr<Asset> _envAtlas;
    std::shared_ptr<Texture> _canvasTexture;
    std::shared_ptr<RenderTarget> _canvasTarget;
    std::shared_ptr<RenderPassFade> _fadePass;
    std::shared_ptr<RenderPassBrush> _brushPass;
    std::shared_ptr<RenderPassDownsample> _preview;
    std::array<std::shared_ptr<BlendState>, 3> _blendStates;
    std::shared_ptr<StandardMaterial> _material;
    Entity* _cube = nullptr;
    std::array<Brush, kMaxBrushes> _brushes{};

    int _count = 5;
    int _size = 28;
    float _speed = 1.0f;
    BlendKind _blend = BlendKind::Add;
    float _trail = 1.5f;
    bool _border = false;
    bool _clearPending = true;  // the first frame clears the canvas
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(RenderPassShaderQuadExample)
