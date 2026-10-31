// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream render-targets/multi-render-targets.
//
// A camera renders a chess board (chess-board.glb, draco compressed) into a render target with
// three colour attachments at once: the lit colour, the world normal (n * 0.5 + 0.5) and the
// gloss. The three 512 x 512 RGBA8 textures (mipmapped, clamped) are then shown on the screen:
// the colour large at the top, the normals bottom left, the gloss bottom right. The target is
// multisampled 2x with a depth buffer; its camera (priority -1, ACES, far clip 500) renders only
// the board's layer, inserted after the world layer, and orbits it at a radius of 110 and a height
// of 45. The normal and gloss attachments clear to colours of their own, (0.5, 0.5, 1) and black,
// what the output writes for empty space. The main camera draws the helipad environment (mip 1)
// and the previews.
//
// The board's materials write the two extra attachments through a chunk override of
// forward-fragment-output, the forward program's output hook, reading the shading normal and gloss
// the lit path leaves behind; a pass drawing into a target with three attachments selects the
// fragment entry that writes three.
//
// DEVIATIONS:
// - No shader passes: the override applies wherever the board's materials draw, and they draw only
//   for the texture camera (the board is on its layer alone), where upstream gates it on the
//   camera's "MyMRT" pass.
// - The previews are TexturePreviewPasses (examples/texturePreview.h), upstream's TextureRenderer.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <cmath>
#include <memory>
#include <vector>

#include "../exampleApp.h"
#include "../texturePreview.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/layer.h"
#include "scene/materials/material.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int LAYERID_RT = 20;

    // The board's output: its lit colour to the first attachment, the world normal to the second
    // and the gloss to the third.
    constexpr const char* kMrtOutput = R"(
void forwardFragmentOutput(inout float4 color, inout ForwardFragmentOutputs outputs)
{
    outputs.color1 = float4(vtOutputNormal * 0.5 + 0.5, 1.0);
    outputs.color2 = float4(float3(vtOutputGloss), 1.0);
    outputs.color3 = color;
    outputs.secondary = float4(0.0, 0.0, 0.0, 0.0);
}
)";
}

class MultiRenderTargetsExample final : public ExampleApp
{
public:
    MultiRenderTargetsExample() : ExampleApp({.title = "Multi Render Targets"}) {}

protected:
    bool create() override
    {
        _board = std::make_unique<Asset>("board", AssetType::CONTAINER, assetPath("models/chess-board.glb"));
        _helipad = std::make_unique<Asset>("helipad-env-atlas", AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        ContainerResource* board = _board->resourceAs<ContainerResource>();
        Texture* helipad = _helipad->resourceAs<Texture>();
        if (!board || !helipad) {
            spdlog::error("Failed to load models/chess-board.glb or cubemaps/helipad-env-atlas.png");
            return false;
        }

        // Set up the skydome
        scene()->setEnvAtlas(helipad);
        scene()->setSkyboxMip(1);

        // A layer for the objects rendered into the texture, right after the world layer
        scene()->layers()->insert(std::make_shared<Layer>("RTLayer", LAYERID_RT), 1);

        // The textures rendered into, and their target with a depth buffer
        for (const char* name : {"RT-texture-0", "RT-texture-1", "RT-texture-2"}) {
            _textures.push_back(createTexture(name, 512, 512));
        }
        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.name = "MRT";
        options.colorBuffers = {_textures[0].get(), _textures[1].get(), _textures[2].get()};
        options.depth = true;
        options.samples = 2;
        _renderTarget = device()->createRenderTarget(options);

        // The texture camera, rendering the RTLayer into the target, first each frame
        Entity* textureCamera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        textureCamera->setName("TextureCamera");
        auto* textureCameraComp = textureCamera->findComponent<CameraComponent>();
        textureCameraComp->setLayers({LAYERID_RT});
        textureCameraComp->setToneMapping(TONEMAP_ACES);
        textureCameraComp->setPriority(-1);
        Camera* cam = textureCameraComp->camera();
        cam->setFarClip(500.0f);
        cam->setRenderTarget(_renderTarget);
        // The colour buffers clear to the camera's clear colour; the normals and gloss buffers
        // clear to what the output writes for empty space.
        cam->setClearColor(1, Color(0.5f, 0.5f, 1.0f, 1.0f));
        cam->setClearColor(2, Color(0.0f, 0.0f, 0.0f, 1.0f));
        _textureCamera = textureCamera;

        // The chess board, rendered into the RTLayer only, its materials writing the extra outputs
        Entity* boardEntity = board->instantiateRenderEntity();
        root()->addChild(boardEntity);
        for (auto* render : boardEntity->findComponents<RenderComponent>()) {
            render->setLayers({LAYERID_RT});
            for (const auto& meshInstance : render->meshInstances()) {
                if (Material* material = meshInstance ? meshInstance->material() : nullptr) {
                    material->setShaderChunk("forward-fragment-output", kMrtOutput);
                }
            }
        }

        // The main camera, drawing the sky and the previews
        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->setLayers({LAYERID_SKYBOX, LAYERID_IMMEDIATE, LAYERID_UI});

        // The three textures, shown after the frame
        for (const auto& texture : _textures) {
            auto preview = std::make_shared<TexturePreviewPass>(device(), texture.get());
            preview->init(nullptr);
            engine()->renderer()->addAppendPass(preview);
            _previews.push_back(preview);
        }
        return true;
    }

    void update(const float dt) override
    {
        _angle += dt;

        // Orbit the camera around the board
        _textureCamera->setLocalPosition(110.0f * std::sin(_angle * 0.2f), 45.0f, 110.0f * std::cos(_angle * 0.2f));
        _textureCamera->lookAt(Vector3(0.0f, 0.0f, 0.0f));

        // The colour large at the top, the normals and the gloss below it
        const auto [width, height] = device()->size();
        const float ratio = static_cast<float>(width) / static_cast<float>(height);
        placePreview(*_previews[0], 0.25f, 0.3f - 0.25f * ratio, 0.5f, 0.5f * ratio);
        placePreview(*_previews[1], 0.025f, 0.75f - 0.225f * ratio, 0.45f, 0.45f * ratio);
        placePreview(*_previews[2], 0.525f, 0.75f - 0.225f * ratio, 0.45f, 0.45f * ratio);
    }

    void destroy() override
    {
        // The passes are registered with the renderer, so they go while the engine is alive.
        if (engine() && engine()->renderer()) {
            for (const auto& preview : _previews) {
                engine()->renderer()->removeAppendPass(preview);
            }
        }
        _previews.clear();
    }

private:
    std::shared_ptr<Texture> createTexture(const char* name, const uint32_t width, const uint32_t height)
    {
        TextureOptions options;
        options.name = name;
        options.width = width;
        options.height = height;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = true;
        options.minFilter = FilterMode::FILTER_LINEAR_MIPMAP_LINEAR;
        options.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device().get(), options);
        texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        return texture;
    }

    // A preview at (x, y) with the size (w, h), as fractions of the back buffer, from its top left.
    void placePreview(TexturePreviewPass& preview, const float x, const float y, const float w, const float h) const
    {
        const auto [width, height] = device()->size();
        const auto fw = static_cast<float>(width);
        const auto fh = static_cast<float>(height);
        preview.setViewport(Vector4(x * fw, y * fh, w * fw, h * fh));
        preview.setScissor(preview.viewport());
    }

    std::unique_ptr<Asset> _board;
    std::unique_ptr<Asset> _helipad;
    std::vector<std::shared_ptr<Texture>> _textures;
    std::shared_ptr<RenderTarget> _renderTarget;
    std::vector<std::shared_ptr<TexturePreviewPass>> _previews;
    Entity* _textureCamera = nullptr;
    float _angle = 1.0f;
};

VISUTWIN_EXAMPLE_MAIN(MultiRenderTargetsExample)
