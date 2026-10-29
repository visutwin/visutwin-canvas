// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream test/taa-alpha.
//
// Checks that a camera frame with TAA keeps the alpha of a render target: a scene camera
// (priority -1, on a layer of its own) renders eight towers of eleven grey boxes (scale 12,
// diffuse 0.52, metalness 0.35, gloss 0.5, each at a random rotation) into an RGBA8 texture
// cleared to a fully TRANSPARENT green, through a camera frame with TAA (jitter 1, sharpness 1),
// ACES and no bloom. A white directional light at (50, 30, 0) (intensity 1.2, 2048 shadows out to
// 800) lights that layer, under an ambient of (0.25, 0.25, 0.28). The display camera, clearing
// to a dull red, shows the texture three times: its colour and its alpha as opaque previews
// across the top, and in the middle the texture blended over the red by its own alpha. Where
// alpha is right the composite shows the towers on red with no green at all; the alpha
// preview is white on the towers and black elsewhere. The scene camera starts at
// (300 sin 0.3, 150, 300 cos 0.3) and orbits the origin, out to 1200.
//
// Controls:
//   T      TAA on / off (sharpness 1 with it, 0 without)
//   - / =  the camera frame's resolution scale, 0.2 to 1
//
// DEVIATIONS:
// - The camera frame renders its scene into RGBA16F (the engine's only camera-frame scene
//   format) where upstream asks for RGBA8; both keep alpha.
// - The previews and the composite are one Slang ShaderMaterial on plane primitives placed in
//   clip space, standing in for upstream's TextureRenderer and its composite shader; the RGB
//   preview writes the texture's bytes as they are (upstream decodes them from gamma and
//   encodes them again, which is the identity).
// - The rotations come from a fixed-seed generator instead of Math.random().
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "platform/input/keyboard.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/layer.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/slangShaders.h"

using namespace visutwin::canvas;

namespace
{
    // The layer the scene camera renders, which the display camera does not.
    constexpr int LAYERID_SCENE_RT = 100;

    // A textured quad placed straight in clip space by its node: a plane primitive's x and -z are
    // the quad's x and y, the node's transform puts it on the screen, and uv = xy + 0.5.
    // Mode 0 shows the texture's colour, mode 1 its alpha, both opaque; mode 2 writes the texel
    // as it is, for the material's blend to composite.
    const char* const kQuadSlang = R"slang(
import vtdraw;
#include "bindings.slang"

struct QuadData
{
    uint mode;
    uint pad0;
    uint pad1;
    uint pad2;
};
VT_DECLARE_MATERIAL_UNIFORMS(QuadData, quad)
VT_DECLARE_baseColorMap

struct VertexIn
{
    [[vk::location(0)]] float3 position : POSITION;
};

struct Varyings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
Varyings quadVertex(VertexIn input)
{
    const float2 xy = float2(input.position.x, -input.position.z);
    Varyings output;
    float4 clip = mul(float4(xy, 0.0, 1.0), drawModel());
    clip.z = 0.5 * (clip.z + clip.w);
    output.position = clip;
    output.uv = xy + 0.5;
    return output;
}

[shader("fragment")]
float4 quadFragment(Varyings input) : SV_Target0
{
    // v = 0 is the texture's top row; the node's negative y scale puts that edge at the top.
    const float4 texel = baseColorMap.Sample(input.uv);
    if (quad.mode == 0u) {
        return float4(texel.rgb, 1.0);
    }
    if (quad.mode == 1u) {
        return float4(texel.aaa, 1.0);
    }
    return texel;
}
)slang";

    class QuadMaterial final : public ShaderMaterial
    {
    public:
        QuadMaterial(const std::string& name, const std::shared_ptr<Shader>& shader, const uint32_t mode)
            : ShaderMaterial(name, shader)
        {
            _data.mode = mode;
        }

        const void* customUniformData(size_t& outSize) const override
        {
            outSize = sizeof(_data);
            return &_data;
        }

    private:
        struct alignas(16) Data
        {
            uint32_t mode = 0;
            uint32_t pad[3] = {0, 0, 0};
        } _data;
    };
}

class TaaAlphaExample final : public ExampleApp
{
public:
    TaaAlphaExample() : ExampleApp({.title = "TAA Alpha", .antialias = false}) {}

protected:
    bool create() override
    {
        const auto layers = scene()->layers();
        _sceneLayer = std::make_shared<Layer>("TaaAlphaReproRT", LAYERID_SCENE_RT);
        layers->insert(_sceneLayer, 1);

        // The towers, on the scene camera's layer
        _cubeMaterial = std::make_shared<StandardMaterial>();
        _cubeMaterial->setDiffuse(Color(0.52f, 0.52f, 0.52f, 1.0f));
        _cubeMaterial->setGloss(0.5f);
        _cubeMaterial->setMetalness(0.35f);
        _cubeMaterial->setUseMetalness(true);

        std::mt19937 random(1);
        std::uniform_real_distribution<float> angle(0.0f, 360.0f);
        constexpr int numTowers = 8;
        constexpr float radius = 200.0f;
        constexpr int numCubes = 12;
        for (int i = 0; i < numTowers; ++i) {
            // Upstream lowers the scale after each tower, but declares it inside the loop, so
            // every cube is 12.
            constexpr float scale = 12.0f;
            const float fraction = static_cast<float>(i) / numTowers * 2.0f * 3.14159265358979f;
            for (int y = 0; y <= 10; ++y) {
                const float elevationRadius = radius * (1.0f - static_cast<float>(y) / numCubes);
                const Vector3 position(elevationRadius * std::sin(fraction), static_cast<float>(y) * 6.0f,
                    elevationRadius * std::cos(fraction));
                Entity* cube = createPrimitive("box", _cubeMaterial.get(), position, Vector3(scale, scale, scale),
                    {LAYERID_SCENE_RT});
                const float ex = angle(random);
                const float ey = angle(random);
                const float ez = angle(random);
                cube->setLocalEulerAngles(ex, ey, ez);
                if (auto* render = cube->findComponent<RenderComponent>()) {
                    render->setCastShadows(true);
                }
            }
        }

        // The light, on the scene camera's layer only
        Entity* light = createDirectionalLight(Vector3(50.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.2f, true);
        if (auto* lightComponent = light->findComponent<LightComponent>()) {
            lightComponent->setShadowDistance(800.0f);
            lightComponent->setShadowResolution(2048);
            lightComponent->setShadowBias(0.2f);
            lightComponent->setShadowNormalBias(0.05f);
            lightComponent->setLayers({LAYERID_SCENE_RT});
        }
        scene()->setAmbientLight(0.25f, 0.25f, 0.28f);

        // The texture the scene camera renders into, sized with the window
        const int panel = panelSize();
        TextureOptions textureOptions;
        textureOptions.name = "TaaAlphaReproSceneColor";
        textureOptions.width = static_cast<uint32_t>(panel);
        textureOptions.height = static_cast<uint32_t>(panel);
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        _sceneColor = std::make_shared<Texture>(device().get(), textureOptions);
        _sceneColor->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _sceneColor->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device().get();
        targetOptions.colorBuffer = _sceneColor.get();
        targetOptions.depth = true;
        targetOptions.samples = 1;
        targetOptions.name = "TaaAlphaReproRT";
        _sceneTarget = device()->createRenderTarget(targetOptions);

        // The scene camera: a fully transparent green clear, so green shows only where alpha is
        // lost
        const Vector3 focus(0.0f, 0.0f, 0.0f);
        _sceneCamera = createCamera(Vector3(300.0f * std::sin(0.3f), 150.0f, 300.0f * std::cos(0.3f)));
        _sceneCamera->setName("SceneCamera");
        _sceneCamera->lookAt(focus);
        _sceneCameraComponent = _sceneCamera->findComponent<CameraComponent>();
        _sceneCameraComponent->setLayers({LAYERID_SCENE_RT});
        _sceneCameraComponent->setPriority(-1);
        _sceneCameraComponent->camera()->setClearColor(Color(0.0f, 1.0f, 0.0f, 0.0f));
        _sceneCameraComponent->camera()->setNearClip(0.5f);
        _sceneCameraComponent->camera()->setFarClip(2000.0f);
        _sceneCameraComponent->camera()->setRenderTarget(_sceneTarget);
        _sceneCameraComponent->setToneMapping(TONEMAP_ACES);
        if (CameraControls* controls = addOrbitControls(_sceneCamera, focus)) {
            controls->setPitchRange(Vector2(-90.0f, 90.0f));
            controls->setZoomRange(Vector2(0.01f, 1200.0f));
            controls->storeResetState();
        }
        applySettings();

        // The display camera: the previews and the composite over a dull red
        Entity* displayCamera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        displayCamera->setName("DisplayCamera");
        auto* displayComponent = displayCamera->findComponent<CameraComponent>();
        displayComponent->setLayers({LAYERID_WORLD, LAYERID_SKYBOX, LAYERID_UI});
        displayComponent->camera()->setClearColor(Color(0.55f, 0.35f, 0.35f, 1.0f));
        displayComponent->camera()->setNearClip(0.1f);
        displayComponent->camera()->setFarClip(100.0f);
        displayComponent->setPriority(0);

        auto shader = getOrCreateSlangShaderFromSource(device().get(), "taa-alpha-quad", kQuadSlang);
        if (!shader) {
            spdlog::error("taa-alpha: the quad shader did not compile");
            return false;
        }
        // The composite first: the previews, made after it, draw over it where they overlap.
        _composite = createQuad(shader, 2, "TaaAlphaComposite");
        _colorPreview = createQuad(shader, 0, "TaaAlphaColor");
        _alphaPreview = createQuad(shader, 1, "TaaAlphaAlpha");
        layoutQuads();

        spdlog::info("Controls: T TAA, -/= resolution scale");
        return true;
    }

    void update(const float /*dt*/) override
    {
        auto* keyboard = engine()->keyboard();
        if (keyboard && keyboard->wasPressed(Key::T)) {
            _taaEnabled = !_taaEnabled;
            applySettings();
        }
        if (keyboard && keyboard->wasPressed(Key::Minus)) {
            _scale = std::max(0.2f, std::round((_scale - 0.1f) * 10.0f) / 10.0f);
            applySettings();
        }
        if (keyboard && keyboard->wasPressed(Key::Equals)) {
            _scale = std::min(1.0f, std::round((_scale + 0.1f) * 10.0f) / 10.0f);
            applySettings();
        }

        // The panel follows the window: half its width, one texel per pixel
        const int panel = panelSize();
        if (_sceneTarget && (_sceneTarget->width() != panel || _sceneTarget->height() != panel)) {
            _sceneTarget->resize(panel, panel);
        }
        layoutQuads();
    }

    void destroy() override
    {
        for (const auto& material : _quadMaterials) {
            material->setBaseColorTexture(nullptr);
        }
        if (_sceneCameraComponent) {
            _sceneCameraComponent->camera()->setRenderTarget(nullptr);
        }
    }

private:
    int panelSize() const
    {
        const auto [width, height] = device()->size();
        return std::max(2, width / 2);
    }

    Entity* createQuad(const std::shared_ptr<Shader>& shader, const uint32_t mode, const char* name)
    {
        auto material = std::make_shared<QuadMaterial>(name, shader, mode);
        material->setAlphaMode(AlphaMode::BLEND);
        material->setCullMode(CullMode::CULLFACE_NONE);
        material->setDepthState(DepthState::noDepth());
        material->setBaseColorTexture(_sceneColor.get());
        _quadMaterials.push_back(material);

        Entity* entity = createPrimitive("plane", material.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(1.0f, 1.0f, 1.0f), {LAYERID_WORLD});
        entity->setName(name);
        if (auto* render = entity->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
            for (auto* meshInstance : render->meshInstances()) {
                // Placed in clip space, it is never in any frustum.
                meshInstance->setCull(false);
            }
        }
        return entity;
    }

    // The placement, in clip space: a preview at (x, y, width, height) in viewport
    // fractions from the top left, and the composite half the window wide, a little above centre.
    void layoutQuads()
    {
        const auto [width, height] = device()->size();
        const float ratio = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        const auto place = [](Entity* entity, const float x, const float y, const float w, const float h) {
            if (entity) {
                entity->setLocalPosition(2.0f * x + w - 1.0f, 1.0f - 2.0f * y - h, 0.0f);
                entity->setLocalScale(2.0f * w, -2.0f * h, 1.0f);
            }
        };
        place(_colorPreview, 0.025f, 0.75f - 0.225f * ratio, 0.45f, 0.45f * ratio);
        place(_alphaPreview, 0.525f, 0.75f - 0.225f * ratio, 0.45f, 0.45f * ratio);
        if (_composite) {
            _composite->setLocalPosition(0.0f, 0.4f, 0.0f);
            _composite->setLocalScale(1.0f, -ratio, 1.0f);
        }
    }

    void applySettings()
    {
        if (!_sceneCameraComponent) {
            return;
        }
        auto taa = _sceneCameraComponent->taa();
        taa.enabled = _taaEnabled;
        taa.jitter = 1.0f;
        _sceneCameraComponent->setTaa(taa);

        auto rendering = _sceneCameraComponent->rendering();
        rendering.renderTargetScale = _scale;
        rendering.samples = 1;
        rendering.bloomIntensity = 0.0f;
        // Sharpened while TAA is on; the camera frame stays active when it is off.
        rendering.sharpness = _taaEnabled ? 1.0f : 0.0f;
        _sceneCameraComponent->setRendering(rendering);
        spdlog::info("taa-alpha: TAA {}, scale {:.1f}", _taaEnabled ? "on" : "off", _scale);
    }

    std::shared_ptr<Layer> _sceneLayer;
    std::shared_ptr<StandardMaterial> _cubeMaterial;
    std::shared_ptr<Texture> _sceneColor;
    std::shared_ptr<RenderTarget> _sceneTarget;
    std::vector<std::shared_ptr<QuadMaterial>> _quadMaterials;
    Entity* _sceneCamera = nullptr;
    CameraComponent* _sceneCameraComponent = nullptr;
    Entity* _colorPreview = nullptr;
    Entity* _alphaPreview = nullptr;
    Entity* _composite = nullptr;
    bool _taaEnabled = true;
    float _scale = 1.0f;
};

VISUTWIN_EXAMPLE_MAIN(TaaAlphaExample)
