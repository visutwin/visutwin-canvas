// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream test/dual-source-blending (a hidden test example).
//
// Dual-source blending: the fragment stage writes a second colour that the blend reads as a factor.
// An overlay quad blends `source0 + destination * source1` over a black-and-white checkerboard,
// with source0 a dark red (0.45, 0.02, 0.02) and source1 green (0, 0.85, 0.18): a black cell gets
// the red alone, a white cell the red plus the green and reads green. The checkerboard
// (checkerboard-black-white.png, nearest filtered, tiled 6 x 4) is an unlit, untonemapped emissive
// plane of 5 x 3.2; the overlay, 3.5 x 2.1, sits 0.01 in front of it. An orthographic camera
// (height 2) at z = 5 clears to (0.02, 0.02, 0.02). On a device without dual-source blending only
// the checkerboard is drawn.
//
// The two colours come from a chunk override of forward-fragment-output, the forward program's
// output hook, as upstream's material overrides its output chunk: a material whose blend state
// reads the second source (BLENDMODE_SRC1_*) gets the fragment entry that writes it.
//
// DEVIATION: one pixel per point (upstream allows a pixel ratio of up to 2).
//
#include <memory>

#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    // The overlay's two outputs: the first colour, and the second source the blend multiplies the
    // destination by.
    constexpr const char* kDualSourceOutput = R"(
void forwardFragmentOutput(inout float4 color, inout ForwardFragmentOutputs outputs)
{
    color = float4(0.45, 0.02, 0.02, 0.0);
    outputs.color1 = color;
    outputs.color2 = color;
    outputs.color3 = color;
    outputs.secondary = float4(0.0, 0.85, 0.18, 1.0);
}
)";
}

class DualSourceBlendingExample final : public ExampleApp
{
public:
    DualSourceBlendingExample() : ExampleApp({.title = "Dual-Source Blending"}) {}

protected:
    bool create() override
    {
        _checkerboard = std::make_unique<Asset>("checkerboard-black-white", AssetType::TEXTURE,
            assetPath("textures/checkerboard-black-white.png"), AssetData{.mipmaps = false});
        Texture* checkerboard = _checkerboard->resourceAs<Texture>();
        if (!checkerboard) {
            spdlog::error("Failed to load textures/checkerboard-black-white.png");
            return false;
        }
        checkerboard->setMinFilter(FilterMode::FILTER_NEAREST);
        checkerboard->setMagFilter(FilterMode::FILTER_NEAREST);

        _backgroundMaterial = std::make_shared<StandardMaterial>();
        _backgroundMaterial->setUseLighting(false);
        _backgroundMaterial->setUseTonemap(false);
        _backgroundMaterial->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _backgroundMaterial->setEmissiveMap(checkerboard);
        _backgroundMaterial->setEmissiveMapTiling(Vector2(6.0f, 4.0f));
        _backgroundMaterial->setCullMode(CullMode::CULLFACE_NONE);

        Entity* background = createPrimitive("plane", _backgroundMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(5.0f, 1.0f, 3.2f));
        background->setName("Checkerboard");
        background->setLocalEulerAngles(90.0f, 0.0f, 0.0f);

        if (device()->supportsDualSourceBlending()) {
            _blendMaterial = std::make_shared<StandardMaterial>();
            _blendMaterial->setUseLighting(false);
            _blendMaterial->setUseTonemap(false);
            _blendMaterial->setShaderChunk("forward-fragment-output", kDualSourceOutput);
            auto blend = std::make_shared<BlendState>();
            blend->setEnabled(true);
            blend->setColorOp(BLENDEQUATION_ADD);
            blend->setColorSrcFactor(BLENDMODE_ONE);
            blend->setColorDstFactor(BLENDMODE_SRC1_COLOR);
            blend->setAlphaOp(BLENDEQUATION_ADD);
            blend->setAlphaSrcFactor(BLENDMODE_ZERO);
            blend->setAlphaDstFactor(BLENDMODE_ONE);
            _blendMaterial->setBlendState(blend);
            _blendMaterial->setCullMode(CullMode::CULLFACE_NONE);
            auto depth = std::make_shared<DepthState>();
            depth->setDepthWrite(false);
            _blendMaterial->setDepthState(depth);

            Entity* overlay = createPrimitive("plane", _blendMaterial.get(), Vector3(0.0f, 0.0f, 0.01f),
                Vector3(3.5f, 1.0f, 2.1f));
            overlay->setName("Dual-source overlay");
            overlay->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        } else {
            spdlog::warn("This device has no dual-source blending; only the checkerboard is drawn");
        }

        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 5.0f));
        Camera* cam = camera->findComponent<CameraComponent>()->camera();
        cam->setClearColor(Color(0.02f, 0.02f, 0.02f, 1.0f));
        cam->setProjection(ProjectionType::Orthographic);
        cam->setOrthoHeight(2.0f);
        return true;
    }

private:
    std::unique_ptr<Asset> _checkerboard;
    std::shared_ptr<StandardMaterial> _backgroundMaterial;
    std::shared_ptr<StandardMaterial> _blendMaterial;
};

VISUTWIN_EXAMPLE_MAIN(DualSourceBlendingExample)
