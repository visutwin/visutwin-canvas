// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.10.2026
//
// SMAA 1x (Subpixel Morphological Anti-Aliasing) as a camera frame effect at the RESOLVE
// stage: it finds the edges in the scene, recognises the shape of each aliased edge, and
// blends the pixels along it with their neighbours by the area the edge before aliasing would
// cover (engine/shaders/slang/programs/smaa.slang, three passes). Unlike TAA it works within
// one frame: no camera jitter, no history, no ghosting, and no detail finer than a pixel
// recovered. The depth of field, bloom and the composition read the anti-aliased scene.
//
// Opt-in, like any registered effect:
//
//     auto smaa = std::make_shared<SmaaEffect>();
//     camera->addFrameEffect(smaa);
//
// The parameters default to the reference implementation's HIGH preset; the presets
// (threshold / maxSearchSteps / maxSearchStepsDiag / cornerRounding) are LOW 0.15 / 4 / 0 /
// 100, MEDIUM 0.1 / 8 / 0 / 100, HIGH 0.1 / 16 / 8 / 25 and ULTRA 0.05 / 32 / 16 / 25. It
// combines with multisampling (RenderingSettings::samples), and with TAA it resolves after it
// (the built-in TAA resolves first).
//
// Debug views (RenderingSettings::composeDebug): "smaaedges", the edges found (red on a
// pixel's left, green on its top), and "smaaweights", how much each pixel blends with its
// neighbours (red along horizontal edges, green along vertical ones). DEVIATION: the blend pass
// writes the view into its output while one is shown, which the composition displays, since a
// compose chunk here takes no texture.
//
// Ported from the reference implementation of SMAA: Copyright (C) 2013 Jorge Jimenez, Jose I.
// Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez; MIT license
// (http://www.iryoku.com/smaa/). See NOTICE.
//
#pragma once

#include <array>
#include <memory>

#include "cameraFrameEffect.h"

namespace visutwin::canvas
{
    class RenderPassShaderQuad;

    class SmaaEffect final : public CameraFrameEffect
    {
    public:
        SmaaEffect();
        ~SmaaEffect() override;

        /// The brightness difference between neighbouring pixels above which they are separated
        /// by an edge, 0-0.5. Lower smooths more edges, at a cost, and can smooth texture detail.
        float threshold = 0.1f;
        /// The steps searching for the end of a horizontal or vertical edge, each covering two
        /// pixels, 1-112. The ends of longer edges are not found, which smooths them less well.
        int maxSearchSteps = 16;
        /// The steps searching for the end of a diagonal edge, 0-20; 0 disables diagonal edges,
        /// which are then smoothed as horizontal and vertical ones.
        int maxSearchStepsDiag = 8;
        /// How much the sharp corners are rounded, 0-100: 0 keeps them the sharpest, 100 smooths
        /// them like the rest of the edges.
        float cornerRounding = 25.0f;

        [[nodiscard]] bool ownsPasses() const override { return true; }
        void createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes) override;
        void destroyPasses() override;
        void update() override;
        void resolveScene(CameraFrameEffectScene& scene) override;
        void frameUpdate(const CameraFrameEffectContext& context) override;

        /// The edges, blending weights and anti-aliased scene, while the passes exist.
        [[nodiscard]] Texture* edgesTexture() const;
        [[nodiscard]] Texture* weightsTexture() const;
        [[nodiscard]] Texture* outputTexture() const;

        /// The lookup textures, created with the passes and kept for the effect's life.
        [[nodiscard]] Texture* areaTexture() const { return _areaTexture.get(); }
        [[nodiscard]] Texture* searchTexture() const { return _searchTexture.get(); }

        /// The uniform block the three passes read: 1 / width, 1 / height, width, height; the
        /// clamped parameters (corner rounding 0-1); x the display exposure, y the debug view.
        struct Uniforms
        {
            float metrics[4];
            float params[4];
            float output[4];
        };

    private:
        class Pass;

        CameraComponent* _camera = nullptr;
        std::shared_ptr<Texture> _areaTexture;
        std::shared_ptr<Texture> _searchTexture;
        std::shared_ptr<Texture> _edges;
        std::shared_ptr<Texture> _weights;
        std::shared_ptr<Texture> _output;
        std::array<std::shared_ptr<Pass>, 3> _passes{};
        Texture* _source = nullptr;
        Uniforms _uniforms{};
    };
}
