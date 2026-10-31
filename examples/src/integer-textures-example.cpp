// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream render-targets/integer-textures.
//
// A falling-sand simulation kept in an UNSIGNED INTEGER texture (PIXELFORMAT_R8U, 512 x 512):
// each texel packs a particle's element (bits 0-2: air, sand, orange sand, grey sand, wall),
// whether it moved this step (bit 3) and its shade (bits 4-7). A quad shader steps the
// simulation four times a frame, reading the state by texel and writing the next state as a
// uint; a second quad shader turns the state into colour in an RGBA8 texture, with a ring for
// the brush, which an unlit emissive plane (10 x 10 at (0, 5, 0)) shows. The camera at (0, 5, 15)
// looks at its centre, under the helipad environment at skybox mip 2. To begin with a wall
// crosses the middle of the lower half and 6% of the texels are sand.
//
// Keys and the mouse stand in for upstream's controls panel:
//   Left mouse      add the brush's element (Shift-left or right mouse removes)
//   1 / 2 / 3 / 4   brush: sand, orange sand, grey sand, stone
//   [ / ]           brush size -/+ 1 (1 .. 16, 8 to begin with)
//   Space           reset
//
// DEVIATIONS:
// - The steps ping-pong between the two state textures (A to B, B to A, twice), where upstream
//   renders each step into B and copies B back into A; the state after each step is the same.
// - The passes render at the start of the frame graph (Renderer::addPrependPass), where upstream
//   draws them from the update.
// - The reset's random sand comes from a fixed-seed generator, so every run starts alike.
// - The shaders take the texture's size from their uniforms, where upstream queries it.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/materials/standardMaterial.h"
#include "scene/renderer/forwardRenderer.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kStepsPerFrame = 4;
    constexpr float kPlaneWidth = 10.0f;
    constexpr float kPlaneHeight = 10.0f;
    constexpr uint32_t kTextureHeight = 512;
    constexpr uint32_t kTextureWidth = static_cast<uint32_t>(kTextureHeight * (kPlaneWidth / kPlaneHeight));

    // The particle encoding both shaders share, and the uniforms they read.
    constexpr const char* kSandCommon = R"slang(
import vtquad;
#include "bindings.slang"

struct SandUniforms
{
    float4 mouse;   // xy: the mouse in uv, z: the brush radius in uv, w: a random value
    uint4 params;   // x: the mouse button (0 none, 1 add, 2 remove), y: the pass number, z: the brush
    uint4 size;     // xy: the state texture's size in texels
};
VT_DECLARE_QUAD_UNIFORMS(SandUniforms, u)
VT_DECLARE_QUAD_UINT_INPUT0(sourceTexture)

static const uint AIR = 0u;
static const uint SAND = 1u;
static const uint ORANGESAND = 2u;
static const uint GRAYSAND = 3u;
static const uint WALL = 4u;

struct Particle
{
    uint element;
    bool movedThisFrame;
    uint shade;
};

bool isInBounds(int2 c, int2 size)
{
    return c.x > 0 && c.x < size.x - 1 && c.y > 0 && c.y < size.y - 1;
}

float rand(float2 pos, float val)
{
    return frac(pos.x * pos.y * val * 1000.0);
}

uint pack(Particle p)
{
    uint packed = p.element & 0x7u;
    packed |= (p.movedThisFrame ? 1u : 0u) << 3;
    packed |= (p.shade & 0xFu) << 4;
    return packed;
}

Particle unpack(uint packed)
{
    Particle p;
    p.element = packed & 0x7u;
    p.movedThisFrame = ((packed >> 3) & 0x1u) != 0u;
    p.shade = (packed >> 4) & 0xFu;
    return p;
}

Particle getParticle(int2 coord)
{
    return unpack(sourceTexture.Load(int3(coord, 0)).x);
}

[shader("vertex")]
QuadVarying sandVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}
)slang";

    // One step: a particle of sand falls into the air below it (or diagonally, by the pass), and
    // the brush writes its element.
    constexpr const char* kSandSimulation = R"slang(
[shader("fragment")]
uint sandFragment(QuadVarying input) : SV_Target0
{
    const int2 size = int2(u.size.xy);
    const int2 coord = int2(input.uv * float2(size));

    // Out of bounds: a wall
    if (!isInBounds(coord, size)) {
        return WALL;
    }

    const float d = distance(u.mouse.xy, input.uv);
    const uint passNum = u.params.y;
    const int dir = int(passNum % 3u) - 1;

    const Particle current = getParticle(coord);
    Particle nextState = current;

    if (u.params.x == 1u && d < u.mouse.z) {
        nextState.element = u.params.z;
        nextState.movedThisFrame = true;
        nextState.shade = uint(rand(input.uv, u.mouse.w * float(passNum)) * 15.0);
    } else if (u.params.x == 2u && d < u.mouse.z) {
        nextState.element = AIR;
        nextState.movedThisFrame = false;
        nextState.shade = uint(rand(input.uv, u.mouse.w * float(passNum)) * 15.0);
    }

    if (current.element == AIR) {
        const Particle above = getParticle(coord + int2(dir, -1));
        if (above.element != AIR && above.element != WALL) {
            nextState = above;
            nextState.movedThisFrame = true;
        }
    } else if (current.element != WALL) {
        const Particle below = getParticle(coord + int2(-dir, 1));
        if (below.element == AIR && !below.movedThisFrame) {
            nextState = below;
            nextState.movedThisFrame = false;
        }
    }

    return pack(nextState);
}
)slang";

    // The state as colour, with a ring where the brush is.
    constexpr const char* kRenderOutput = R"slang(
static const float3 whiteColor = float3(1.0, 1.0, 1.0);
static const float3 skyBlueColor = float3(0.2, 0.2, 0.2);
static const float3 yellowSandColor = float3(0.73, 0.58, 0.26);
static const float3 orangeSandColor = float3(0.87, 0.43, 0.22);
static const float3 graySandColor = float3(0.13, 0.16, 0.17);
static const float3 grayWallColor = float3(0.5, 0.5, 0.5);
static const float circleOutline = 0.0025;

[shader("fragment")]
float4 outputFragment(QuadVarying input) : SV_Target0
{
    const int2 size = int2(u.size.xy);
    const int2 coord = int2(input.uv * float2(size));
    const Particle particle = getParticle(coord);

    float3 gameColor = skyBlueColor;
    const float shade = (float(particle.shade) / 15.0) * 0.5;
    if (particle.element == SAND) {
        gameColor = lerp(yellowSandColor, whiteColor, shade);
    } else if (particle.element == WALL) {
        gameColor = grayWallColor;
    } else if (particle.element == ORANGESAND) {
        gameColor = lerp(orangeSandColor, whiteColor, shade);
    } else if (particle.element == GRAYSAND) {
        gameColor = lerp(graySandColor, whiteColor, shade);
    }

    // The brush ring
    const float radius = u.mouse.z;
    const float d = length(input.uv - u.mouse.xy);
    const float wd = fwidth(d);
    const float circleVal = smoothstep(radius + wd, radius, d);
    const float circleInner = smoothstep(radius - circleOutline + wd, radius - circleOutline, d);
    const float brush = max(circleVal - circleInner, 0.0) * 0.5;

    return float4(lerp(gameColor, float3(1.0, 1.0, 1.0), brush), 1.0);
}
)slang";

    struct SandUniforms
    {
        float mouse[4];
        uint32_t params[4];
        uint32_t size[4];
    };

    // A quad pass over one of the two shaders, reading the state texture `source`.
    class SandPass final : public RenderPassShaderQuad
    {
    public:
        SandPass(const std::shared_ptr<GraphicsDevice>& device, const bool output, Texture* source)
            : RenderPassShaderQuad(device), _output(output), _source(source) {}

        SandUniforms uniforms{};

        void prepareShaders() override
        {
            if (!shader()) {
                if (_output) {
                    static const std::string source = std::string(kSandCommon) + kRenderOutput;
                    useSlangSource("integer-textures-output", source);
                } else {
                    static const std::string source = std::string(kSandCommon) + kSandSimulation;
                    useSlangSource("integer-textures-sand", source);
                }
            }
        }

        void execute() override
        {
            prepareShaders();
            setQuadUniforms(uniforms);
            setQuadTextureBinding(0, _source);
            RenderPassShaderQuad::execute();
        }

    private:
        bool _output;
        Texture* _source;
    };
}

class IntegerTexturesExample final : public ExampleApp
{
public:
    IntegerTexturesExample() : ExampleApp({.title = "Integer Textures"}) {}

protected:
    bool create() override
    {
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        Texture* helipad = _helipad->resourceAs<Texture>();
        if (!helipad) {
            spdlog::error("Failed to load cubemaps/helipad-env-atlas.png");
            return false;
        }

        // The two integer state textures and their targets, and the colour output
        for (int i = 0; i < 2; ++i) {
            _state[i] = createTexture(i == 0 ? "PixelBuffer_0" : "PixelBuffer_1", PixelFormat::PIXELFORMAT_R8U);
            _stateTargets[i] = createTarget(i == 0 ? "PixelRenderTarget_0" : "PixelRenderTarget_1", _state[i].get());
        }
        _output = createTexture("OutputTexture", PixelFormat::PIXELFORMAT_RGBA8);
        _output->setAddressU(ADDRESS_REPEAT);
        _output->setAddressV(ADDRESS_REPEAT);
        _outputTarget = createTarget("PixelRenderTarget_2", _output.get());

        resetData();

        // The steps alternate between the two textures, ending in the first, which the output reads
        for (int i = 0; i < kStepsPerFrame; ++i) {
            auto pass = std::make_shared<SandPass>(device(), false, _state[i % 2].get());
            pass->init(_stateTargets[(i + 1) % 2]);
            _steps.push_back(pass);
            engine()->renderer()->addPrependPass(pass);
        }
        _outputPass = std::make_shared<SandPass>(device(), true, _state[kStepsPerFrame % 2].get());
        _outputPass->init(_outputTarget);
        engine()->renderer()->addPrependPass(_outputPass);

        // Set up the skydome
        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxMip(2);
        scene()->setExposure(1.0f);

        Entity* camera = createCamera(Vector3(0.0f, 5.0f, 15.0f));
        camera->lookAt(Vector3(0.0f, 5.0f, 0.0f));
        _camera = camera->findComponent<CameraComponent>();
        _camera->camera()->setFarClip(500.0f);

        // The plane showing the simulation
        _screenMaterial = std::make_shared<StandardMaterial>();
        _screenMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        _screenMaterial->setEmissiveMap(_output.get());
        _screenMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _screenMaterial->setUseLighting(false);
        Entity* screen = createPrimitive("plane", _screenMaterial.get(), Vector3(0.0f, 5.0f, 0.0f),
            Vector3(kPlaneWidth, 1.0f, kPlaneHeight));
        screen->setName("gameScreen");
        screen->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

        spdlog::info("Mouse: left adds, shift-left or right removes. Keys: 1-4 brush, [ ] brush size, Space reset");
        return true;
    }

    void update(float /*dt*/) override
    {
        if (const auto* keyboard = engine()->keyboard()) {
            if (keyboard->wasPressed(Key::Space)) {
                resetData();
            }
            constexpr std::array keys{Key::Digit1, Key::Digit2, Key::Digit3, Key::Digit4};
            for (size_t i = 0; i < keys.size(); ++i) {
                if (keyboard->wasPressed(keys[i])) {
                    _brush = static_cast<uint32_t>(i + 1);
                }
            }
            if (keyboard->wasPressed(Key::LeftBracket)) {
                _brushSize = std::max(_brushSize - 1, 1);
            } else if (keyboard->wasPressed(Key::RightBracket)) {
                _brushSize = std::min(_brushSize + 1, 16);
            }
        }

        uint32_t mouseState = 0;
        if (const auto* mouse = engine()->mouse()) {
            const auto* keyboard = engine()->keyboard();
            const bool shift = keyboard && (keyboard->isPressed(Key::LeftShift) || keyboard->isPressed(Key::RightShift));
            if (mouse->isPressed(MouseButton::Left)) {
                mouseState = shift ? 2 : 1;
            } else if (mouse->isPressed(MouseButton::Right)) {
                mouseState = 2;
            }
            updateMousePosition(mouse->x(), mouse->y());
        }

        const float brushRadius = static_cast<float>(_brushSize) / static_cast<float>(std::max(kTextureWidth, kTextureHeight));
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        for (const auto& pass : _steps) {
            SandUniforms& u = pass->uniforms;
            u.mouse[0] = _mouseUv[0];
            u.mouse[1] = _mouseUv[1];
            u.mouse[2] = brushRadius;
            u.mouse[3] = unit(_random);
            u.params[0] = mouseState;
            u.params[1] = _passNum;
            u.params[2] = _brush;
            u.size[0] = kTextureWidth;
            u.size[1] = kTextureHeight;
            _passNum = (_passNum + 1) % 16;
        }
        SandUniforms& u = _outputPass->uniforms;
        u.mouse[0] = _mouseUv[0];
        u.mouse[1] = _mouseUv[1];
        u.mouse[2] = brushRadius;
        u.size[0] = kTextureWidth;
        u.size[1] = kTextureHeight;
    }

    void destroy() override
    {
        // The passes are registered with the renderer, so they go while the engine is alive.
        if (engine() && engine()->renderer()) {
            for (const auto& pass : _steps) {
                engine()->renderer()->removePrependPass(pass);
            }
            if (_outputPass) {
                engine()->renderer()->removePrependPass(_outputPass);
            }
        }
        _steps.clear();
        _outputPass.reset();
    }

private:
    std::shared_ptr<Texture> createTexture(const char* name, const PixelFormat format)
    {
        TextureOptions options;
        options.name = name;
        options.width = kTextureWidth;
        options.height = kTextureHeight;
        options.format = format;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device().get(), options);
        texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        return texture;
    }

    std::shared_ptr<RenderTarget> createTarget(const char* name, Texture* colorBuffer)
    {
        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.name = name;
        options.colorBuffer = colorBuffer;
        options.depth = false;
        return device()->createRenderTarget(options);
    }

    // The initial state: a wall across the middle of the lower half, sand sprinkled on 6% of the
    // texels with a random shade, air elsewhere. Written into the texture the first step reads.
    void resetData()
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<uint8_t> data(static_cast<size_t>(kTextureWidth) * kTextureHeight, 0);
        const auto w = static_cast<float>(kTextureWidth);
        const auto h = static_cast<float>(kTextureHeight);
        for (uint32_t x = 0; x < kTextureWidth; ++x) {
            for (uint32_t y = 0; y < kTextureHeight; ++y) {
                const size_t i = static_cast<size_t>(y) * kTextureWidth + x;
                const auto fx = static_cast<float>(x);
                const auto fy = static_cast<float>(y);
                const bool isDefaultWall = fx > w * 0.3f && fx < w * 0.7f && fy > h * 0.7f && fy < h * 0.8f;
                if (isDefaultWall) {
                    data[i] = 4;
                } else if (unit(_random) > 0.94f) {
                    data[i] = static_cast<uint8_t>(1u | (static_cast<uint32_t>(unit(_random) * 15.0f) << 4));
                }
            }
        }
        _state[0]->setLevelData(0, data.data(), data.size());
        _state[0]->upload();
    }

    // The mouse in the plane's uv (v = 0 at its top), by the ray through it to the plane's
    // depth.
    void updateMousePosition(const float x, const float y)
    {
        Camera* cam = _camera->camera();
        const Vector3 nearPoint = _camera->screenToWorld(x, y, cam->nearClip());
        const Vector3 farPoint = _camera->screenToWorld(x, y, cam->farClip());
        const float dz = nearPoint.getZ() - farPoint.getZ();
        if (std::abs(dz) < 1e-6f) {
            return;
        }
        const float t = nearPoint.getZ() / dz;
        const float px = nearPoint.getX() + (farPoint.getX() - nearPoint.getX()) * t;
        const float py = nearPoint.getY() + (farPoint.getY() - nearPoint.getY()) * t;
        _mouseUv[0] = (kPlaneWidth * 0.5f + px) / kPlaneWidth;
        _mouseUv[1] = (kPlaneHeight - py) / kPlaneHeight;
    }

    std::unique_ptr<Asset> _helipad;
    std::array<std::shared_ptr<Texture>, 2> _state;
    std::array<std::shared_ptr<RenderTarget>, 2> _stateTargets;
    std::shared_ptr<Texture> _output;
    std::shared_ptr<RenderTarget> _outputTarget;
    std::vector<std::shared_ptr<SandPass>> _steps;
    std::shared_ptr<SandPass> _outputPass;
    std::shared_ptr<StandardMaterial> _screenMaterial;
    CameraComponent* _camera = nullptr;
    std::mt19937 _random{12345u};
    float _mouseUv[2] = {0.0f, 0.0f};
    uint32_t _passNum = 0;
    uint32_t _brush = 1;
    int _brushSize = 8;
};

VISUTWIN_EXAMPLE_MAIN(IntegerTexturesExample)
