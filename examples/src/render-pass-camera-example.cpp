// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2026
//
// Port of upstream render-targets/render-pass-camera.
//
// Replaces the rendering of a camera with its own render passes (CameraComponent::
// setRenderPasses). A RenderPassForward renders the World and UI layers with the camera into a
// texture that follows the size of the window, and a shader-quad pass then draws it to the
// screen, tinted from yellow to cyan and back. The camera circles the chess board (radius 110,
// height 45) looking at a point sliding along it, under the helipad env atlas (mip 2) at
// exposure 5; the board casts and receives no shadows.
//
// Model: "Chess Board" by Idmental, CC BY 4.0
// (https://sketchfab.com/3d-models/chess-board-901eeeca884f4622ac37b7e8f7cb82c3).
//
// DEVIATIONS:
// - the tint shader is Slang compiled at run time for either backend, where upstream writes GLSL
//   and WGSL.
//
#include <cmath>
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/handlers/containerResource.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "scene/layer.h"
#include "scene/renderer/forwardRenderer.h"
#include "scene/renderer/renderPassForward.h"

using namespace visutwin::canvas;

namespace
{
    // The scene texture times the tint.
    const char* const kTintSlang = R"slang(
import vtquad;
#include "bindings.slang"

struct TintUniforms
{
    float4 tint;  // rgb
};
VT_DECLARE_QUAD_UNIFORMS(TintUniforms, u)
VT_DECLARE_QUAD_INPUT0(sourceTexture)

[shader("vertex")]
QuadVarying tintVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 tintFragment(QuadVarying input) : SV_Target0
{
    const float4 color = sourceTexture.Sample(input.uv);
    return float4(color.rgb * u.tint.rgb, color.a);
}
)slang";

    struct TintUniforms
    {
        float tint[4];
    };

    // A quad pass drawing the source texture tinted.
    class RenderPassTint final : public RenderPassShaderQuad
    {
    public:
        RenderPassTint(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture)
            : RenderPassShaderQuad(device), _sourceTexture(sourceTexture)
        {
        }

        Color tint{1.0f, 1.0f, 1.0f, 1.0f};

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("render-pass-camera-tint", kTintSlang);
            }
        }

        void execute() override
        {
            prepareShaders();
            setQuadUniforms(TintUniforms{{tint.r, tint.g, tint.b, 1.0f}});
            setQuadTextureBinding(0, _sourceTexture);
            RenderPassShaderQuad::execute();
        }

    private:
        Texture* _sourceTexture;
    };
}

class RenderPassCameraExample final : public ExampleApp
{
public:
    RenderPassCameraExample() : ExampleApp({.title = "Render Pass Camera"}) {}

protected:
    bool create() override
    {
        _envAtlas = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _boardAsset = std::make_unique<Asset>("chess-board", AssetType::CONTAINER,
            assetPath("models/chess-board.glb"));
        Texture* envAtlas = _envAtlas->resourceAs<Texture>();
        auto* board = _boardAsset->resourceAs<ContainerResource>();
        if (!envAtlas || !board) {
            spdlog::error("render-pass-camera needs the helipad env atlas and chess-board.glb");
            return false;
        }
        scene()->setEnvAtlas(envAtlas);
        scene()->setSkyboxMip(2);
        scene()->setExposure(5.0f);

        // The chess board, which neither casts nor receives shadows.
        Entity* boardEntity = board->instantiateRenderEntity();
        boardEntity->setEngine(engine());
        for (auto* render : boardEntity->findComponents<RenderComponent>()) {
            render->setCastShadows(false);
            render->setReceiveShadows(false);
        }
        root()->addChild(boardEntity);

        _cameraEntity = createCamera(Vector3(0.0f, 30.0f, -60.0f));
        _cameraEntity->lookAt(Vector3(0.0f, 0.0f, 100.0f));
        auto* camera = _cameraEntity->findComponent<CameraComponent>();
        camera->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        camera->camera()->setFarClip(500.0f);

        // The scene renders into a texture first; the pass resizes it to the window.
        TextureOptions textureOptions;
        textureOptions.name = "RTTexture";
        textureOptions.width = 4;
        textureOptions.height = 4;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        _texture = std::make_shared<Texture>(device().get(), textureOptions);
        _texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        _texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device().get();
        targetOptions.colorBuffer = _texture.get();
        targetOptions.depth = true;
        targetOptions.name = "RTTarget";
        _target = device()->createRenderTarget(targetOptions);

        // The forward pass renders the World and UI layers with the camera into the texture,
        // following the size of the back buffer (no resize source).
        const auto& layers = scene()->layers();
        _scenePass = std::make_shared<RenderPassForward>(device(), layers.get(), scene().get(),
            engine()->renderer().get(), camera);
        _scenePass->init(_target, std::make_shared<RenderPassOptions>());
        _scenePass->addLayer(layers->getLayerById(LAYERID_WORLD).get(), false);
        _scenePass->addLayer(layers->getLayerById(LAYERID_UI).get(), true);

        // The tint pass draws the texture to the back buffer.
        _tintPass = std::make_shared<RenderPassTint>(device(), _texture.get());
        _tintPass->init(nullptr);

        // The two passes replace the camera's default rendering.
        camera->setRenderPasses({_scenePass, _tintPass});
        return true;
    }

    void update(const float dt) override
    {
        if (!_cameraEntity || !_tintPass) {
            return;
        }
        _angle += dt;

        // The focus slides along the board as the camera circles it.
        const Vector3 focus(0.0f, 10.0f, std::sin(1.0f + _angle * 0.3f) * 90.0f);
        _cameraEntity->setLocalPosition(110.0f * std::sin(_angle * 0.2f), 45.0f, 110.0f * std::cos(_angle * 0.2f));
        _cameraEntity->lookAt(focus);

        // From yellow to cyan and back.
        const float t = std::sin(_angle * 0.5f) * 0.5f + 0.5f;
        _tintPass->tint = Color(1.0f - t, 1.0f, t, 1.0f);
    }

    void destroy() override
    {
        if (_cameraEntity) {
            if (auto* camera = _cameraEntity->findComponent<CameraComponent>()) {
                camera->setRenderPasses({});
            }
        }
        _scenePass.reset();
        _tintPass.reset();
    }

private:
    std::unique_ptr<Asset> _envAtlas;
    std::unique_ptr<Asset> _boardAsset;
    std::shared_ptr<Texture> _texture;
    std::shared_ptr<RenderTarget> _target;
    std::shared_ptr<RenderPassForward> _scenePass;
    std::shared_ptr<RenderPassTint> _tintPass;
    Entity* _cameraEntity = nullptr;
    float _angle = 3.0f;
};

VISUTWIN_EXAMPLE_MAIN(RenderPassCameraExample)
