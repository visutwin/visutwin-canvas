// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
// Port of upstream camera-frame/ink-outline.
//
// A CameraFrameEffect with a render pass of its own. The ink outline draws lines where the depth
// or the brightness of the opaque scene changes abruptly. Its pass runs after the opaque geometry
// and before the transparent geometry (the postOpaque stage), reading the copy of the opaque scene
// the camera frame takes for it, and drawing the lines into the scene. The soap bubbles and the
// fireflies are transparent, so they render after it and are not outlined.
//
// The house (scaled 100x) under the helipad env atlas (skybox mip 2, exposure 1.4), a warm
// shadow-casting directional light, 20 iridescent bubbles rising around the house and 120
// additive fireflies; neutral tone mapping and bloom 0.02. The camera orbits the house's bounds
// centre (0, 9.17, 0) from (28, 14, 28), out to 120.
//
// Keys stand in for upstream's controls panel, over the same parameters:
//   O      outline on / off
//   M      MSAA (4 samples) on / off
//   T      TAA on / off
//   [ / ]  line width -/+ 1 pixel (1..4)
//
// DEVIATIONS:
// - the depth is device depth, linearised in the shader with the camera's clip planes.
// - the bubbles' iridescence has no minimum thickness (only a thickness map would read it).
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "core/math/curve.h"
#include "core/math/curveSet.h"
#include "framework/assets/asset.h"
#include "framework/components/particlesystem/particleSystemComponent.h"
#include "framework/components/particlesystem/particleSystemComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "scene/constants.h"
#include "scene/graphics/cameraFrameEffect.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // The ink pass's shader, Slang written as an engine quad program is, compiled at run time
    // for whichever backend runs. Input 0 is the copy of the opaque scene (linear HDR, sampled at
    // LOD 0: the copy is mipmapped), input 1 the scene depth (point sampled).
    const char* const kInkOutlineSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct InkOutlineUniforms
{
    float4 params;        // x: line width (pixels), y: depth threshold, z: colour threshold, w: intensity
    float4 color;         // rgb: the ink
    float4 texelSize;     // xy: one pixel of the scene, in uv
    float4 cameraParams;  // x: near clip, y: far clip, z: 1 when orthographic
};
VT_DECLARE_QUAD_UNIFORMS(InkOutlineUniforms, u)
VT_DECLARE_QUAD_INPUT0(sceneColorMap)
VT_DECLARE_QUAD_DEPTH_INPUT1(sceneDepthMap)

float inverseDepth(float2 uv)
{
    const float depth = sceneDepthMap.SampleLevel(uv, 0.0);
    return 1.0 / quadLinearDepth(depth, u.cameraParams.x, u.cameraParams.y, u.cameraParams.z > 0.5);
}

float sceneLuminance(float2 uv)
{
    return dot(sceneColorMap.SampleLevel(uv, 0.0).rgb, float3(0.2126, 0.7152, 0.0722));
}

float contrast(float a, float b)
{
    return abs(a - b) / (a + b + 0.05);
}

// The centre of the texel a position is in.
float2 snapToTexel(float2 uv)
{
    return (floor(uv / u.texelSize.xy) + 0.5) * u.texelSize.xy;
}

// The ink at a texel centre, comparing the samples a and b away on both diagonals.
float inkAt(float2 uv, float2 a, float2 b)
{
    // The inverse of the depth changes linearly across a flat surface, so its second difference
    // is zero there, and lines appear at silhouettes and creases only. Relative to the depth, it
    // does not depend on the distance.
    const float center = inverseDepth(uv);
    const float depthEdge = (abs(inverseDepth(uv + a) + inverseDepth(uv - a) - 2.0 * center) +
                             abs(inverseDepth(uv + b) + inverseDepth(uv - b) - 2.0 * center)) / center;

    // Abrupt changes of the brightness, such as the outlines of the shadows.
    const float colorEdge = max(contrast(sceneLuminance(uv + a), sceneLuminance(uv - a)),
                                contrast(sceneLuminance(uv + b), sceneLuminance(uv - b)));

    return max(smoothstep(u.params.y, u.params.y * 2.0, depthEdge),
               smoothstep(u.params.z, u.params.z * 2.0, colorEdge));
}

[shader("vertex")]
QuadVarying inkOutlineVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 inkOutlineFragment(QuadVarying input) : SV_Target0
{
    const float2 uv = input.uv;
    const float2 a = u.texelSize.xy * u.params.x;
    const float2 b = float2(a.x, -a.y);

    // The ink averaged over four positions spread across the pixel in a rotated grid, scaled
    // with the width of the lines, so the lines fade out over a pixel or more at their edges
    // instead of stepping between the pixels. The depth is read from the nearest texel, so the
    // samples compared are a whole number of texels away from a texel centre: on a flat surface
    // they are the same distance away on both sides, and their second difference stays zero.
    const float ink = (inkAt(snapToTexel(uv + a * float2(-0.375, -0.125)), a, b) +
                       inkAt(snapToTexel(uv + a * float2(0.125, -0.375)), a, b) +
                       inkAt(snapToTexel(uv + a * float2(0.375, 0.125)), a, b) +
                       inkAt(snapToTexel(uv + a * float2(-0.125, 0.375)), a, b)) * 0.25 * u.params.w;
    if (ink <= 0.0) {
        discard;
    }

    const float4 scene = sceneColorMap.SampleLevel(uv, 0.0);
    return float4(lerp(scene.rgb, u.color.rgb, ink), scene.a);
}
)slang";

    struct InkOutlineUniforms
    {
        float params[4];
        float color[4];
        float texelSize[4];
        float cameraParams[4];
    };

    // A full screen pass drawing ink lines into the scene. Pixels without a line are discarded,
    // so they stay as they were.
    class RenderPassInkOutline final : public RenderPassShaderQuad
    {
    public:
        RenderPassInkOutline(const std::shared_ptr<GraphicsDevice>& device, CameraComponent* camera,
            std::function<Texture*()> sceneColorGrab, Texture* depth)
            : RenderPassShaderQuad(device), _camera(camera), _sceneColorGrab(std::move(sceneColorGrab)),
              _depth(depth)
        {
        }

        // the width of the lines, in whole pixels
        float width = 1.0f;
        // how sensitive the lines are to changes of the depth, and of the brightness
        float depthThreshold = 0.0f;
        float colorThreshold = 0.0f;
        // how dark the lines are, 0-1
        float intensity = 1.0f;
        Color color;
        // the size of a pixel of the scene, in uv space
        float texelWidth = 0.0f;
        float texelHeight = 0.0f;

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("ink-outline", kInkOutlineSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            Texture* scene = _sceneColorGrab ? _sceneColorGrab() : nullptr;
            const Camera* camera = _camera ? _camera->camera() : nullptr;
            if (!shader() || !scene || !_depth || !camera) {
                return;
            }

            InkOutlineUniforms uniforms{};
            uniforms.params[0] = std::max(std::round(width), 1.0f);
            uniforms.params[1] = depthThreshold;
            uniforms.params[2] = colorThreshold;
            uniforms.params[3] = intensity;
            uniforms.color[0] = color.r;
            uniforms.color[1] = color.g;
            uniforms.color[2] = color.b;
            uniforms.texelSize[0] = texelWidth;
            uniforms.texelSize[1] = texelHeight;
            // the camera planes, which reading the depth depends on
            uniforms.cameraParams[0] = camera->nearClip();
            uniforms.cameraParams[1] = camera->farClip();
            uniforms.cameraParams[2] = camera->projection() == ProjectionType::Orthographic ? 1.0f : 0.0f;
            setQuadUniforms(uniforms);
            setQuadTextureBinding(0, scene);
            setQuadTextureBinding(1, _depth);
            RenderPassShaderQuad::execute();
        }

    private:
        CameraComponent* _camera;
        std::function<Texture*()> _sceneColorGrab;
        Texture* _depth;
    };

    // The effect owns the pass: it creates it in createPasses and lets go of it in destroyPasses.
    // The resources it requires are what the camera frame provides to it:
    // - the copy of the opaque scene colour, which the frame takes because the effect requires it,
    // - a render target onto the scene colour, which the pass draws the lines into,
    // - the scene depth, which at this point holds the depth of the opaque geometry.
    // The pass is added to the postOpaque stage: after the opaque geometry, before the transparent
    // geometry, which renders over the lines.
    class InkOutlineEffect final : public CameraFrameEffect
    {
    public:
        InkOutlineEffect()
            : CameraFrameEffect("inkOutline",
                  {FrameResource::SceneColorGrab, FrameResource::SceneTarget, FrameResource::Depth})
        {
        }

        // The width of the lines, in whole pixels
        float width = 3.0f;
        // How sensitive the lines are to changes of the depth - lower values draw more lines
        float depthThreshold = 0.002f;
        // How sensitive the lines are to changes of the brightness - lower values draw more lines
        float colorThreshold = 0.48f;
        // How dark the lines are, 0-1
        float intensity = 1.0f;
        // The colour of the ink
        Color color = Color(0.02f, 0.02f, 0.05f, 1.0f);

        bool ownsPasses() const override { return true; }

        void createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes) override
        {
            _pass = std::make_shared<RenderPassInkOutline>(resources.device, resources.camera,
                resources.sceneColorGrab, resources.depth);
            _pass->init(resources.sceneTarget);
            passes.postOpaque.push_back(_pass);
        }

        void destroyPasses() override { _pass.reset(); }

        // called every frame, before the pass renders
        void update() override
        {
            if (!_pass) {
                return;
            }
            _pass->width = width;
            _pass->depthThreshold = depthThreshold;
            _pass->colorThreshold = colorThreshold;
            _pass->intensity = intensity;
            _pass->color = color;
        }

        // called every frame, with the size of the scene this frame
        void frameUpdate(const CameraFrameEffectContext& frame) override
        {
            if (_pass && frame.sceneWidth > 0 && frame.sceneHeight > 0) {
                _pass->texelWidth = 1.0f / static_cast<float>(frame.sceneWidth);
                _pass->texelHeight = 1.0f / static_cast<float>(frame.sceneHeight);
            }
        }

    private:
        std::shared_ptr<RenderPassInkOutline> _pass;
    };

    struct Bubble
    {
        Entity* entity = nullptr;
        float angle = 0.0f;
        float distance = 0.0f;
        float height = 0.0f;
        float speed = 0.0f;
    };
}

class InkOutlineExample final : public ExampleApp
{
public:
    // The scene is rendered to an antialiased texture, so the back buffer is not.
    InkOutlineExample() : ExampleApp({.title = "Ink Outline Example", .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ParticleSystemComponentSystem>();
    }

    bool create() override
    {
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _house = std::make_unique<Asset>("house", AssetType::CONTAINER, assetPath("models/house.glb"));
        _spark = std::make_unique<Asset>("spark", AssetType::TEXTURE, assetPath("textures/spark.png"));

        // The sky and the image based lighting
        if (Texture* envAtlas = _helipad->resourceAs<Texture>()) {
            scene()->setEnvAtlas(envAtlas);
        }
        scene()->setSkyboxMip(2);
        scene()->setExposure(1.4f);

        // The house, a white model which the outlines turn into a drawing
        auto* houseContainer = _house->resourceAs<ContainerResource>();
        if (!houseContainer) {
            spdlog::error("ink-outline needs models/house.glb");
            return false;
        }
        auto* house = houseContainer->instantiateRenderEntity();
        house->setLocalScale(100, 100, 100);
        root()->addChild(house);

        // A warm directional light casting shadows, whose outlines are drawn as well
        auto* light = createDirectionalLight(Vector3(-55.0f, 0.0f, -30.0f), Color(1.0f, 0.92f, 0.8f, 1.0f), 2.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowDistance(80.0f);
            lightComp->setShadowResolution(2048);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        // Soap bubbles rising around the house, each tinted a little differently. They are
        // transparent, so they render after the outlines.
        const Color bubbleTints[] = {
            Color(1.0f, 0.7f, 0.85f, 1.0f), Color(0.7f, 0.85f, 1.0f, 1.0f),
            Color(0.75f, 1.0f, 0.8f, 1.0f), Color(1.0f, 0.95f, 0.7f, 1.0f)};
        for (int i = 0; i < 20; ++i) {
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(bubbleTints[i % 4]);
            material->setOpacity(0.3f);
            material->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
            material->setTransparent(true);
            material->setUseMetalness(true);
            material->setMetalness(0.0f);
            material->setGloss(0.95f);
            material->setOpacityFadesSpecular(false);
            material->setIridescenceIntensity(1.0f);
            material->setIridescenceIOR(1.33f);
            material->setIridescenceThicknessMax(600.0f + random() * 400.0f);
            auto depth = std::make_shared<DepthState>();
            depth->setDepthTest(true);
            depth->setDepthWrite(false);
            material->setDepthState(depth);
            _bubbleMaterials.push_back(material);

            auto* bubble = createPrimitive("sphere", material.get());
            bubble->setName("Bubble");
            if (auto* render = bubble->findComponent<RenderComponent>()) {
                render->setCastShadows(false);
            }
            const float size = 2.0f + random() * 3.6f;
            bubble->setLocalScale(size, size, size);
            Bubble entry;
            entry.entity = bubble;
            entry.angle = random() * 2.0f * PI;
            entry.distance = 5.0f + random() * 12.0f;
            entry.height = random() * 20.0f;
            entry.speed = 0.8f + random() * 1.2f;
            _bubbles.push_back(entry);
        }

        // Fireflies drifting around the house, transparent as well
        if (Texture* spark = _spark->resourceAs<Texture>()) {
            auto* fireflies = new Entity();
            fireflies->setEngine(engine());
            fireflies->setName("Fireflies");
            root()->addChild(fireflies);
            fireflies->setLocalPosition(0.0f, 6.0f, 0.0f);
            auto* particles = static_cast<ParticleSystemComponent*>(fireflies->addComponent<ParticleSystemComponent>());
            auto& o = particles->options();
            o.numParticles = 120;
            o.lifetime = 5.0f;
            o.lifetime2 = 5.0f;
            o.rate = 0.04f;
            o.emitterExtents = Vector3(30.0f, 10.0f, 30.0f);
            o.blendType = ParticleBlendType::BLEND_ADDITIVE;
            o.colorMap = spark;
            o.intensity = 6.0f;
            o.scaleGraph = Curve(std::vector<float>{0.0f, 1.0f, 0.5f, 1.6f, 1.0f, 1.0f});
            o.colorGraph = CurveSet({{0.0f, 0.0f, 0.2f, 1.0f, 0.8f, 1.0f, 1.0f, 0.0f},
                                     {0.0f, 0.0f, 0.2f, 0.85f, 0.8f, 0.85f, 1.0f, 0.0f},
                                     {0.0f, 0.0f, 0.2f, 0.3f, 0.8f, 0.3f, 1.0f, 0.0f}});
            o.localVelocityGraph = CurveSet({{0.0f, -1.0f}, {0.0f, -0.5f}, {0.0f, -1.0f}});
            o.localVelocityGraph2 = CurveSet({{0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 1.0f}});
            particles->apply();
        }

        // The camera, orbiting the house
        auto* camera = createCamera(Vector3(28.0f, 14.0f, 28.0f));
        _cameraComp = camera->findComponent<CameraComponent>();
        if (!_cameraComp || !_cameraComp->camera()) {
            return false;
        }
        _cameraComp->camera()->setFarClip(500.0f);
        _cameraComp->camera()->setFov(50.0f);
        if (auto* controls = addOrbitControls(camera, entityBounds(house).center())) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.01f, 120.0f));
            controls->storeResetState();
        }

        // The camera frame: neutral tone mapping, bloom 0.02, and the ink outline effect
        _cameraComp->setToneMapping(TONEMAP_NEUTRAL);
        auto rendering = _cameraComp->rendering();
        rendering.bloomIntensity = 0.02f;
        _cameraComp->setRendering(rendering);

        _inkOutline = std::make_shared<InkOutlineEffect>();
        _cameraComp->addFrameEffect(_inkOutline);

        spdlog::info("Controls: O outline, M MSAA, T TAA, [ ] line width");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type != SDL_EVENT_KEY_DOWN || !_cameraComp) {
            return false;
        }
        switch (event.key.key) {
        case SDLK_O:
            _inkOutline->setEnabled(!_inkOutline->enabled());
            break;
        case SDLK_M: {
            auto rendering = _cameraComp->rendering();
            rendering.samples = rendering.samples > 1 ? 1 : 4;
            _cameraComp->setRendering(rendering);
            break;
        }
        case SDLK_T: {
            auto taa = _cameraComp->taa();
            taa.enabled = !taa.enabled;
            _cameraComp->setTaa(taa);
            break;
        }
        case SDLK_LEFTBRACKET:
            _inkOutline->width = std::max(1.0f, _inkOutline->width - 1.0f);
            break;
        case SDLK_RIGHTBRACKET:
            _inkOutline->width = std::min(4.0f, _inkOutline->width + 1.0f);
            break;
        default:
            return false;
        }
        spdlog::info("outline={} width={:.1f} msaa={} taa={}", _inkOutline->enabled() ? "ON" : "OFF",
            _inkOutline->width, _cameraComp->rendering().samples > 1 ? "ON" : "OFF",
            _cameraComp->taa().enabled ? "ON" : "OFF");
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;

        // The bubbles rise, swaying, and start again at the bottom
        for (const auto& bubble : _bubbles) {
            const float height = std::fmod(bubble.height + _time * bubble.speed, 20.0f);
            const float angle = bubble.angle + _time * 0.1f;
            const float sway = std::sin(_time * 1.5f + bubble.angle * 5.0f) * 0.5f;
            bubble.entity->setLocalPosition(std::cos(angle) * bubble.distance + sway, height,
                std::sin(angle) * bubble.distance);
        }
    }

private:
    // A pseudo random number generator, so that the scene is the same on every run
    float random()
    {
        _seed = (_seed * 16807) % 2147483647;
        return static_cast<float>(static_cast<double>(_seed - 1) / 2147483646.0);
    }

    std::unique_ptr<Asset> _helipad;
    std::unique_ptr<Asset> _house;
    std::unique_ptr<Asset> _spark;

    CameraComponent* _cameraComp = nullptr;
    std::shared_ptr<InkOutlineEffect> _inkOutline;
    std::vector<std::shared_ptr<StandardMaterial>> _bubbleMaterials;
    std::vector<Bubble> _bubbles;
    int64_t _seed = 3;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(InkOutlineExample)
