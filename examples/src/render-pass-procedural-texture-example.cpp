// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2026
//
// Port of upstream render-targets/render-pass-procedural-texture.
//
// Generates the textures of a material on the GPU with two render passes attached to no camera.
// The first renders an animated, tiling domain-warped noise into a texture, its colour in rgb and
// the surface height in alpha; the second renders a normal map from that height (a Sobel filter).
// Both run at the start of the frame, before the cameras sample them: every frame while the
// pattern animates, and otherwise only when a setting changes. Their mipmaps are regenerated each
// time they render. A box, a sphere resting on it (its texture tiled twice around) and a 3 x 3
// ground use the textures, under the helipad env atlas (mip 2, intensity 0.7), a directional
// light (1.2, PCF3 at 2048, distance 12) and ACES. The two textures are shown in the bottom right
// corner.
//
// Keys stand in for upstream's controls panel:
//   Space  animate on / off
//   [ / ]  animation speed -/+ 0.5 (0.1 .. 4)
//   P      palette: agate, malachite, marble
//   S / A  noise scale +/- 1 (1 .. 10)
//   B / V  bumpiness +/- 0.25 (0 .. 3)
//   T      texture resolution, cycled 128 .. 2048 (512 to begin with)
//   H      the texture previews on / off
//
// DEVIATIONS:
// - the passes render as passes at the START of the frame graph (Renderer::addPrependPass),
//   enabled only when their output changes, where upstream renders them from the "prerender"
//   event: that event fires before a frame has begun here, where a pass cannot record on Vulkan.
// - the colour texture is RGBA16F on both backends (both render to half float); upstream falls
//   back to an sRGB 8-bit format, which this engine does not have.
// - upstream's TextureRenderer is a RenderPassDownsample per preview, appended to the frame with
//   the rectangle as its viewport; it copies the texture's values as they are, as upstream's does.
// - upstream's orbit-camera script is CameraControls in orbit mode with the same pitch and
//   distance ranges; anisotropy is the engine's material sampler's, not the textures' own 8.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/assets/asset.h"
#include "framework/components/render/primitiveGeometry.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassDownsample.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/renderer/forwardRenderer.h"

using namespace visutwin::canvas;

namespace
{
    // The animated pattern: colour in rgb, height in alpha. The noise lattice wraps by its
    // period, so the texture tiles.
    const char* const kPatternSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct PatternUniforms
{
    float4 params;      // x: time, y: noise cells across the texture
    float4 colorBase;   // the colours, linear
    float4 colorMid;
    float4 colorHigh;
    float4 colorVein;
};
VT_DECLARE_QUAD_UNIFORMS(PatternUniforms, u)

// GLSL's mod, which stays positive for a negative x (Slang's fmod truncates).
float2 glslMod(float2 x, float y)
{
    return x - y * floor(x / y);
}

// A pseudo-random unit vector for a cell of the lattice, wrapped by the period.
float2 gradient(float2 cell, float period)
{
    cell = glslMod(cell, period);
    const float angle = frac(sin(dot(cell, float2(127.1, 311.7))) * 43758.5453) * 6.2831853;
    return float2(cos(angle), sin(angle));
}

// Gradient noise in -0.7 .. 0.7, tiling with the period.
float noise(float2 p, float period)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    const float a = dot(gradient(i, period), f);
    const float b = dot(gradient(i + float2(1.0, 0.0), period), f - float2(1.0, 0.0));
    const float c = dot(gradient(i + float2(0.0, 1.0), period), f - float2(0.0, 1.0));
    const float d = dot(gradient(i + float2(1.0, 1.0), period), f - float2(1.0, 1.0));
    return lerp(lerp(a, b, w.x), lerp(c, d, w.x), w.y);
}

// Five octaves of the noise, doubling the frequency and the period, so every octave tiles.
float fbm(float2 p, float period)
{
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 5; i++) {
        sum += amplitude * noise(p, period);
        p *= 2.0;
        period *= 2.0;
        amplitude *= 0.5;
    }
    return sum;
}

[shader("vertex")]
QuadVarying patternVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 patternFragment(QuadVarying input) : SV_Target0
{
    const float scale = u.params.y;
    const float2 p = input.uv * scale;
    const float t = u.params.x;

    // The domain warped by the noise itself, twice; offsets keep the noise tiling.
    const float2 q = float2(fbm(p, scale), fbm(p + float2(5.2, 1.3), scale));
    const float2 r = float2(fbm(p + 4.0 * q + float2(1.7, 9.2) + 0.15 * t, scale),
                            fbm(p + 4.0 * q + float2(8.3, 2.8) + 0.126 * t, scale));
    const float h = fbm(p + 4.0 * r, scale);

    const float vein = smoothstep(0.35, 0.6, abs(r.x));
    float3 color = lerp(u.colorBase.rgb, u.colorMid.rgb, clamp(h * h * 6.0, 0.0, 1.0));
    color = lerp(color, u.colorHigh.rgb, clamp(length(q) * 1.5, 0.0, 1.0));
    color = lerp(color, u.colorVein.rgb, vein);

    // The height the normal map is made from, the veins carved into it.
    const float height = clamp(0.6 + 0.3 * h - 0.25 * vein, 0.0, 1.0);
    return float4(color, height);
}
)slang";

    // The normal map from the height in the alpha of input 0, read through the texture's own
    // repeating sampler, so the edges take their neighbours from the opposite side.
    const char* const kNormalMapSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct NormalMapUniforms
{
    float4 params;  // xy: one texel in uv, z: the bumpiness
};
VT_DECLARE_QUAD_UNIFORMS(NormalMapUniforms, u)
VT_DECLARE_QUAD_INPUT0(heightTexture)

float heightAt(float2 uv, float2 offset)
{
    return heightTexture.SampleLevel(uv + offset * u.params.xy, 0.0).a;
}

[shader("vertex")]
QuadVarying normalMapVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 normalMapFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = input.uv;
    const float tl = heightAt(uv, float2(-1.0, 1.0));
    const float t = heightAt(uv, float2(0.0, 1.0));
    const float tr = heightAt(uv, float2(1.0, 1.0));
    const float l = heightAt(uv, float2(-1.0, 0.0));
    const float r = heightAt(uv, float2(1.0, 0.0));
    const float bl = heightAt(uv, float2(-1.0, -1.0));
    const float b = heightAt(uv, float2(0.0, -1.0));
    const float br = heightAt(uv, float2(1.0, -1.0));

    // The Sobel slope along the texture axes, per unit of uv, so the bumps do not depend on
    // the resolution.
    float2 slope = float2((tr + 2.0 * r + br) - (tl + 2.0 * l + bl), (tl + 2.0 * t + tr) - (bl + 2.0 * b + br));
    slope /= 8.0 * u.params.xy;

    const float3 normal = normalize(float3(-slope * u.params.z, 1.0));
    return float4(normal * 0.5 + 0.5, 1.0);
}
)slang";

    struct PatternUniforms
    {
        float params[4];
        float colors[4][4];
    };

    struct NormalMapUniforms
    {
        float params[4];
    };

    // The colours of the pattern, sRGB: base, mid, high and veins.
    struct Palette
    {
        const char* name;
        std::array<uint32_t, 4> colors;
    };
    constexpr std::array<Palette, 3> kPalettes{{
        {"agate", {0x2a1640, 0xc45a2c, 0xf2c58a, 0xf7eee2}},
        {"malachite", {0x06291f, 0x127a52, 0x7fd1a4, 0x021710}},
        {"marble", {0xc9c2b6, 0x8f8273, 0xebe5da, 0x2a211b}},
    }};
    constexpr std::array<int, 5> kResolutions{128, 256, 512, 1024, 2048};

    class RenderPassPattern final : public RenderPassShaderQuad
    {
    public:
        explicit RenderPassPattern(const std::shared_ptr<GraphicsDevice>& device) : RenderPassShaderQuad(device) {}

        float time = 0.0f;
        // whole, so the noise tiles
        float scale = 4.0f;
        // linear
        std::array<std::array<float, 3>, 4> colors{};

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("render-pass-procedural-pattern", kPatternSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            PatternUniforms uniforms{};
            uniforms.params[0] = time;
            uniforms.params[1] = scale;
            for (size_t i = 0; i < colors.size(); ++i) {
                std::copy(colors[i].begin(), colors[i].end(), uniforms.colors[i]);
            }
            setQuadUniforms(uniforms);
            RenderPassShaderQuad::execute();
        }
    };

    class RenderPassNormalMap final : public RenderPassShaderQuad
    {
    public:
        RenderPassNormalMap(const std::shared_ptr<GraphicsDevice>& device, Texture* heightTexture)
            : RenderPassShaderQuad(device), _heightTexture(heightTexture)
        {
            // The texture repeats, so the Sobel taps at an edge wrap round to the other side.
            setQuadTextureOwnSampler(0, true);
        }

        float bumpiness = 1.0f;

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("render-pass-procedural-normals", kNormalMapSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            NormalMapUniforms uniforms{};
            uniforms.params[0] = 1.0f / static_cast<float>(_heightTexture->width());
            uniforms.params[1] = 1.0f / static_cast<float>(_heightTexture->height());
            uniforms.params[2] = bumpiness;
            setQuadUniforms(uniforms);
            setQuadTextureBinding(0, _heightTexture);
            RenderPassShaderQuad::execute();
        }

    private:
        Texture* _heightTexture;
    };
}

class RenderPassProceduralTextureExample final : public ExampleApp
{
public:
    RenderPassProceduralTextureExample() : ExampleApp({.title = "Render Pass Procedural Texture"}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* envAtlas = _envAtlas->resourceAs<Texture>();
        if (!envAtlas) {
            spdlog::error("render-pass-procedural-texture needs the helipad env atlas");
            return false;
        }
        scene()->setEnvAtlas(envAtlas);
        scene()->setSkyboxMip(2);
        scene()->setSkyboxIntensity(0.7f);

        // ------ The textures the passes render, and their targets (no depth) ------
        const int size = kResolutions[_resolutionIndex];
        _colorTexture = createTexture("PatternColor", PixelFormat::PIXELFORMAT_RGBA16F, size);
        _normalTexture = createTexture("PatternNormals", PixelFormat::PIXELFORMAT_RGBA8, size);
        _colorTarget = createTarget("PatternColor", _colorTexture.get());
        _normalTarget = createTarget("PatternNormals", _normalTexture.get());

        // ------ The passes, at the start of every frame, enabled when their output changes ------
        _patternPass = std::make_shared<RenderPassPattern>(device());
        _patternPass->init(_colorTarget);
        _normalMapPass = std::make_shared<RenderPassNormalMap>(device(), _colorTexture.get());
        _normalMapPass->init(_normalTarget);
        // The quads cover the targets, so the previous content is cleared, not loaded.
        const Color black(0.0f, 0.0f, 0.0f, 1.0f);
        _patternPass->setClearColor(&black);
        _normalMapPass->setClearColor(&black);
        engine()->renderer()->addPrependPass(_patternPass);
        engine()->renderer()->addPrependPass(_normalMapPass);

        // ------ The scene ------
        _cubeMaterial = createMaterial(1.0f, 1.0f);
        _sphereMaterial = createMaterial(2.0f, 1.0f);
        _groundMaterial = createMaterial(3.0f, 3.0f);

        Entity* cube = createPrimitive("box", _cubeMaterial.get(), Vector3(0.0f, 0.5f, 0.0f));
        cube->setName("Cube");
        cube->setLocalEulerAngles(0.0f, 30.0f, 0.0f);

        // A finely tessellated sphere resting on the cube, its poles facing sideways.
        _sphereMesh = createMeshFromGeometry(device(), createSphereGeometry(0.5f, 64, 64));
        auto* sphere = new Entity();
        sphere->setName("Sphere");
        sphere->setEngine(engine());
        if (auto* render = static_cast<RenderComponent*>(sphere->addComponent<RenderComponent>())) {
            render->addMeshInstance(std::make_unique<MeshInstance>(_sphereMesh.get(), _sphereMaterial.get(), sphere));
        }
        sphere->setLocalPosition(0.0f, 1.5f, 0.0f);
        sphere->setLocalEulerAngles(0.0f, 0.0f, 90.0f);
        root()->addChild(sphere);
        _objects = {cube, sphere};

        Entity* ground = createPrimitive("plane", _groundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(3.0f, 1.0f, 3.0f));
        ground->setName("Ground");
        if (auto* render = ground->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        Entity* lightEntity = createDirectionalLight(Vector3(50.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f),
            1.2f, true);
        if (auto* light = lightEntity->findComponent<LightComponent>()) {
            light->setShadowType(SHADOW_PCF3_32F);
            light->setShadowResolution(2048);
            light->setShadowDistance(12.0f);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.05f);
        }

        Entity* camera = createCamera(Vector3(0.4f, 2.2f, 4.6f));
        if (auto* component = camera->findComponent<CameraComponent>()) {
            component->setToneMapping(TONEMAP_ACES);
            component->camera()->setFarClip(100.0f);
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.4f, 0.9f, 0.0f))) {
            controls->setEnableFly(false);
            controls->setPitchRange(Vector2(-80.0f, -5.0f));
            controls->setZoomRange(Vector2(2.0f, 12.0f));
            controls->storeResetState();
        }

        // ------ The previews in the bottom right corner ------
        _colorPreview = std::make_shared<RenderPassDownsample>(device(), _colorTexture.get());
        _normalPreview = std::make_shared<RenderPassDownsample>(device(), _normalTexture.get());
        for (const auto& preview : {_colorPreview, _normalPreview}) {
            preview->init(nullptr);
            preview->setRequiresCubemaps(false);
            engine()->renderer()->addAppendPass(preview);
        }

        applySettings();
        spdlog::info("Keys: Space animate, [ ] speed, P palette, S/A scale, B/V bumpiness, T resolution, H previews");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_patternPass) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_SPACE: _animate = !_animate; break;
        case SDLK_LEFTBRACKET: _speed = std::max(0.1f, _speed - 0.5f); break;
        case SDLK_RIGHTBRACKET: _speed = std::min(4.0f, _speed + 0.5f); break;
        case SDLK_P: _paletteIndex = (_paletteIndex + 1) % kPalettes.size(); _patternDirty = true; break;
        case SDLK_S: _scale = std::min(10, _scale + 1); _patternDirty = true; break;
        case SDLK_A: _scale = std::max(1, _scale - 1); _patternDirty = true; break;
        case SDLK_B: _bumpiness = std::min(3.0f, _bumpiness + 0.25f); _normalsDirty = true; break;
        case SDLK_V: _bumpiness = std::max(0.0f, _bumpiness - 0.25f); _normalsDirty = true; break;
        case SDLK_T: {
            // Resizing the targets resizes their textures, which loses their content.
            _resolutionIndex = (_resolutionIndex + 1) % kResolutions.size();
            const int resolution = kResolutions[_resolutionIndex];
            _colorTarget->resize(resolution, resolution);
            _normalTarget->resize(resolution, resolution);
            _patternDirty = true;
            break;
        }
        case SDLK_H: _preview = !_preview; break;
        default: return false;
        }
        applySettings();
        spdlog::info("animate {}, speed {}, palette {}, scale {}, bumpiness {}, resolution {}, previews {}",
            _animate ? "on" : "off", _speed, kPalettes[_paletteIndex].name, _scale, _bumpiness,
            kResolutions[_resolutionIndex], _preview ? "on" : "off");
        return true;
    }

    void update(const float dt) override
    {
        if (!_patternPass) {
            return;
        }
        if (_animate) {
            _patternPass->time += dt * _speed;
            _patternDirty = true;
        }

        // The passes render this frame only when their output changes; the normals follow the
        // pattern.
        _patternPass->setEnabled(_patternDirty);
        if (_patternDirty) {
            _normalsDirty = true;
            _patternDirty = false;
        }
        _normalMapPass->setEnabled(_normalsDirty);
        _normalsDirty = false;

        // Slowly spin the objects around their own axis.
        for (size_t i = 0; i < _objects.size(); ++i) {
            _objects[i]->rotateLocal(0.0f, (10.0f + 4.0f * static_cast<float>(i)) * dt, 0.0f);
        }

        // Square previews at the bottom right, sized by the shorter side of the screen.
        _colorPreview->setEnabled(_preview);
        _normalPreview->setEnabled(_preview);
        if (_preview) {
            const auto [width, height] = device()->size();
            const float side = std::floor(0.2f * static_cast<float>(std::min(width, height)));
            const float margin = std::floor(0.1f * side);
            const float y = static_cast<float>(height) - side - margin;
            _colorPreview->setViewport(Vector4(static_cast<float>(width) - 2.0f * (side + margin), y, side, side));
            _colorPreview->setScissor(_colorPreview->viewport());
            _normalPreview->setViewport(Vector4(static_cast<float>(width) - (side + margin), y, side, side));
            _normalPreview->setScissor(_normalPreview->viewport());
        }
    }

    void destroy() override
    {
        // The passes are registered with the renderer, so they go while the engine is alive.
        if (engine() && engine()->renderer()) {
            engine()->renderer()->removePrependPass(_patternPass);
            engine()->renderer()->removePrependPass(_normalMapPass);
            engine()->renderer()->removeAppendPass(_colorPreview);
            engine()->renderer()->removeAppendPass(_normalPreview);
        }
        _patternPass.reset();
        _normalMapPass.reset();
        _colorPreview.reset();
        _normalPreview.reset();
    }

private:
    std::shared_ptr<Texture> createTexture(const char* name, const PixelFormat format, const int size)
    {
        TextureOptions options;
        options.name = name;
        options.width = static_cast<uint32_t>(size);
        options.height = static_cast<uint32_t>(size);
        options.format = format;
        // regenerated after each pass rendering to the texture
        options.mipmaps = true;
        options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device().get(), options);
        // the pattern tiles, and repeats over the objects
        texture->setAddressU(ADDRESS_REPEAT);
        texture->setAddressV(ADDRESS_REPEAT);
        return texture;
    }

    std::shared_ptr<RenderTarget> createTarget(const char* name, Texture* colorBuffer)
    {
        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.colorBuffer = colorBuffer;
        options.depth = false;
        options.name = name;
        return device()->createRenderTarget(options);
    }

    // A material using the generated textures, tiled by the given amount.
    std::shared_ptr<StandardMaterial> createMaterial(const float tilingU, const float tilingV)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuseMap(_colorTexture.get());
        material->setDiffuseMapTiling(Vector2(tilingU, tilingV));
        material->setNormalMap(_normalTexture.get());
        material->setNormalMapTiling(Vector2(tilingU, tilingV));
        material->setGloss(0.6f);
        material->setUseMetalness(true);
        material->setMetalness(0.0f);
        return material;
    }

    void applySettings()
    {
        _patternPass->scale = static_cast<float>(_scale);
        const auto& palette = kPalettes[_paletteIndex].colors;
        for (size_t i = 0; i < palette.size(); ++i) {
            // sRGB to linear, as upstream's Color.linear (gamma 2.2)
            const float rgb[3] = {static_cast<float>((palette[i] >> 16) & 0xff) / 255.0f,
                static_cast<float>((palette[i] >> 8) & 0xff) / 255.0f, static_cast<float>(palette[i] & 0xff) / 255.0f};
            for (int c = 0; c < 3; ++c) {
                _patternPass->colors[i][static_cast<size_t>(c)] = std::pow(rgb[c], 2.2f);
            }
        }
        // The slopes get steeper with the scale, so the bumpiness is scaled down by it.
        _normalMapPass->bumpiness = 0.12f * _bumpiness / static_cast<float>(_scale);
    }

    std::unique_ptr<Asset> _envAtlas;
    std::shared_ptr<Texture> _colorTexture;
    std::shared_ptr<Texture> _normalTexture;
    std::shared_ptr<RenderTarget> _colorTarget;
    std::shared_ptr<RenderTarget> _normalTarget;
    std::shared_ptr<RenderPassPattern> _patternPass;
    std::shared_ptr<RenderPassNormalMap> _normalMapPass;
    std::shared_ptr<RenderPassDownsample> _colorPreview;
    std::shared_ptr<RenderPassDownsample> _normalPreview;
    std::shared_ptr<StandardMaterial> _cubeMaterial;
    std::shared_ptr<StandardMaterial> _sphereMaterial;
    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<Mesh> _sphereMesh;
    std::vector<Entity*> _objects;

    bool _animate = true;
    float _speed = 1.0f;
    size_t _paletteIndex = 0;  // agate
    int _scale = 2;
    float _bumpiness = 2.0f;
    size_t _resolutionIndex = 2;  // 512
    bool _preview = true;
    bool _patternDirty = true;
    bool _normalsDirty = true;
};

VISUTWIN_EXAMPLE_MAIN(RenderPassProceduralTextureExample)
