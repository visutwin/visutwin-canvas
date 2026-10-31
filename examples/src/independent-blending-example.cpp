// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream test/independent-blending (a hidden test example).
//
// Independent blending: each colour attachment of a render target takes its own blend state and
// colour write mask (BlendState::setAttachment). A dark grey base quad (0.25, 1.8 across) is drawn
// into both attachments of a 512 x 512 two-attachment target, and a semi-transparent light grey
// overlay (0.6, alpha 0.5, 1.2 across) is blended over it. Attachment 0 alpha-blends and writes
// every channel, so its overlay stays grey (0.425); attachment 1 adds and writes red alone, so its
// overlay turns red (0.85, 0.25, 0.25). The two attachments are shown side by side, attachment 0
// on the left, each 1.6 across with 0.1 between them.
//
// The attachments are cleared to colours of their own (RenderPass::setClearColor with an
// attachment index) by a clear-only pass at the start of every frame: attachment 0 dark blue, 1
// dark green, visible as the border around each panel. The camera drawing the quads into the
// target (priority -1, orthographic, height 1, on a layer of its own) does not clear colour. The
// materials write both attachments through a chunk override of forward-fragment-output, the
// forward program's output hook. The display camera, orthographic, clears to (0.02, 0.02, 0.02)
// and is fitted to the panels at any aspect ratio.
//
// DEVIATIONS:
// - No unsigned-integer clear indicator: the engine has no integer pixel formats yet, so the
//   small panel testing an R32U target's clear value is left out.
// - The clear-only pass is a prepend pass of the frame graph (Renderer::addPrependPass), where
//   upstream renders it from the "prerender" event; it clears the depth as well, so the camera
//   pass after it starts from a cleared depth whether or not the two merge.
// - One pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/composition/layerComposition.h"
#include "scene/layer.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int LAYERID_OFFSCREEN = 20;

    // The panels are laid out side by side in world space, and the camera is fitted around them.
    constexpr float kPanelSize = 1.6f;
    constexpr float kPanelGap = 0.1f;
    constexpr float kPanelX = (kPanelSize + kPanelGap) * 0.5f;
    constexpr float kMargin = 1.06f;

    // A material writing the same colour to both attachments. A material drawing into the
    // target writes all of them; the forward program's default copies its colour to each.
    std::string writingOutput(const float r, const float g, const float b, const float a)
    {
        const std::string color = "float4(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
            std::to_string(b) + ", " + std::to_string(a) + ")";
        return "void forwardFragmentOutput(inout float4 color, inout ForwardFragmentOutputs outputs)\n"
               "{\n"
               "    color = " + color + ";\n"
               "    outputs.color1 = color;\n"
               "    outputs.color2 = color;\n"
               "    outputs.color3 = color;\n"
               "    outputs.secondary = float4(0.0, 0.0, 0.0, 0.0);\n"
               "}\n";
    }
}

class IndependentBlendingExample final : public ExampleApp
{
public:
    IndependentBlendingExample() : ExampleApp({.title = "Independent Blending"}) {}

protected:
    bool create() override
    {
        if (!device()->supportsIndependentBlending()) {
            spdlog::warn("This device has no independent blending: both attachments blend as attachment 0");
        }

        // ------ Multiple render target with two color attachments ------
        _attachment0 = createAttachment("attachment0");
        _attachment1 = createAttachment("attachment1");
        RenderTargetOptions options;
        options.graphicsDevice = device().get();
        options.name = "MRT";
        options.colorBuffers = {_attachment0.get(), _attachment1.get()};
        options.depth = true;
        _renderTarget = device()->createRenderTarget(options);

        // ------ Scene rendered into the MRT ------
        // The quads rendered into the MRT are on their own layer, so that the camera rendering
        // them does not also pick up the quads which display the results.
        scene()->layers()->insert(std::make_shared<Layer>("Offscreen", LAYERID_OFFSCREEN), 1);

        // Opaque grey base, identical in both attachments
        auto baseMaterial = createWritingMaterial(0.25f, 0.25f, 0.25f, 1.0f);
        Entity* base = createPrimitive("plane", baseMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(1.8f, 1.0f, 1.8f), {LAYERID_OFFSCREEN});
        base->setName("Base");
        base->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

        // Semi-transparent light grey overlay, blended differently into each attachment. A
        // neutral colour makes the write mask plain: the attachment writing only red turns red.
        auto overlayMaterial = createWritingMaterial(0.6f, 0.6f, 0.6f, 0.5f);
        // Attachment 0: alpha blending, writing every channel: 0.5 * 0.6 + 0.5 * 0.25 = 0.425
        auto blendState = std::make_shared<BlendState>(BlendState::alphaBlend());
        // Attachment 1: additive, writing red only: red 0.6 + 0.25 = 0.85, green and blue left
        // at the base's 0.25
        BlendState attachment1State;
        attachment1State.setEnabled(true);
        attachment1State.setColorOp(BLENDEQUATION_ADD);
        attachment1State.setColorSrcFactor(BLENDMODE_ONE);
        attachment1State.setColorDstFactor(BLENDMODE_ONE);
        attachment1State.setAlphaOp(BLENDEQUATION_ADD);
        attachment1State.setAlphaSrcFactor(BLENDMODE_ONE);
        attachment1State.setAlphaDstFactor(BLENDMODE_ZERO);
        attachment1State.setGreenWrite(false);
        attachment1State.setBlueWrite(false);
        attachment1State.setAlphaWrite(false);
        blendState->setAttachment(1, attachment1State);
        overlayMaterial->setBlendState(blendState);
        overlayMaterial->setTransparent(true);
        auto depth = std::make_shared<DepthState>();
        depth->setDepthWrite(false);
        overlayMaterial->setDepthState(depth);
        Entity* overlay = createPrimitive("plane", overlayMaterial.get(), Vector3(0.0f, 0.0f, 0.01f),
            Vector3(1.2f, 1.0f, 1.2f), {LAYERID_OFFSCREEN});
        overlay->setName("Overlay");
        overlay->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

        // The attachments are cleared by a clear-only pass, each to a colour of its own. The
        // colours stay visible around each panel, as the quads do not cover the attachments.
        _clearPass = std::make_shared<RenderPass>(device());
        _clearPass->init(_renderTarget);
        _clearPass->setClearColor(Color(0.0f, 0.0f, 0.25f, 1.0f), 0);
        _clearPass->setClearColor(Color(0.0f, 0.25f, 0.0f, 1.0f), 1);
        constexpr float clearDepth = 1.0f;
        _clearPass->setClearDepth(&clearDepth);
        engine()->renderer()->addPrependPass(_clearPass);

        // The camera renders over the cleared attachments without clearing colour, before the
        // main camera, so the displayed attachments hold this frame's result.
        Entity* offscreenCamera = createCamera(Vector3(0.0f, 0.0f, 5.0f));
        offscreenCamera->setName("Offscreen Camera");
        auto* offscreen = offscreenCamera->findComponent<CameraComponent>();
        offscreen->setPriority(-1);
        offscreen->setLayers({LAYERID_OFFSCREEN});
        offscreen->camera()->setClearColorBuffer(false);
        offscreen->camera()->setProjection(ProjectionType::Orthographic);
        offscreen->camera()->setOrthoHeight(1.0f);
        offscreen->camera()->setRenderTarget(_renderTarget);

        // ------ Display of both attachments, side by side ------
        // Left: attachment 0, right: attachment 1
        createDisplay("Display 0", _attachment0.get(), -kPanelX);
        createDisplay("Display 1", _attachment1.get(), kPanelX);

        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 5.0f));
        _camera = camera->findComponent<CameraComponent>();
        _camera->setLayers({LAYERID_WORLD});
        _camera->camera()->setClearColor(Color(0.02f, 0.02f, 0.02f, 1.0f));
        _camera->camera()->setProjection(ProjectionType::Orthographic);
        fitCamera();
        return true;
    }

    void update(float /*dt*/) override
    {
        fitCamera();
    }

    void destroy() override
    {
        // The clear pass is registered with the renderer, so it goes while the engine is alive.
        if (_clearPass) {
            engine()->renderer()->removePrependPass(_clearPass);
        }
        _clearPass.reset();
    }

private:
    std::shared_ptr<Texture> createAttachment(const char* name)
    {
        TextureOptions options;
        options.name = name;
        options.width = 512;
        options.height = 512;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        return std::make_shared<Texture>(device().get(), options);
    }

    std::shared_ptr<StandardMaterial> createWritingMaterial(const float r, const float g, const float b,
        const float a)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setUseLighting(false);
        material->setUseTonemap(false);
        material->setCullMode(CullMode::CULLFACE_NONE);
        material->setShaderChunk("forward-fragment-output", writingOutput(r, g, b, a));
        _materials.push_back(material);
        return material;
    }

    void createDisplay(const char* name, Texture* texture, const float x)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setUseLighting(false);
        material->setUseTonemap(false);
        material->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        material->setEmissiveMap(texture);
        material->setCullMode(CullMode::CULLFACE_NONE);
        _materials.push_back(material);

        Entity* entity = createPrimitive("plane", material.get(), Vector3(x, 0.0f, 0.0f),
            Vector3(kPanelSize, 1.0f, kPanelSize), {LAYERID_WORLD});
        entity->setName(name);
        entity->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
    }

    // Keeps the panels fully visible at any aspect ratio: an orthographic camera sees
    // 2 * orthoHeight high and 2 * orthoHeight * aspect wide, so the height is whichever of the
    // two constraints is tighter, with a small margin.
    void fitCamera()
    {
        constexpr float contentWidth = kPanelSize * 2.0f + kPanelGap;
        constexpr float contentHalfHeight = kPanelSize * 0.5f;
        const auto [width, height] = engine()->canvasSize();
        const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
        _camera->camera()->setOrthoHeight(std::max(contentWidth / (2.0f * aspect), contentHalfHeight) * kMargin);
    }

    std::shared_ptr<Texture> _attachment0;
    std::shared_ptr<Texture> _attachment1;
    std::shared_ptr<RenderTarget> _renderTarget;
    std::shared_ptr<RenderPass> _clearPass;
    CameraComponent* _camera = nullptr;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
};

VISUTWIN_EXAMPLE_MAIN(IndependentBlendingExample)
