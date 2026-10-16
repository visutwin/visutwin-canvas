// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// The compose effects of a camera frame (CameraFrameEffect with a ComposeSlot) turned into
// what the compose pass needs: the text of compose/compose-effects.slang, which defines the
// four slot hooks and the effects' debug views in place of the identity file of the tree,
// and the layout of the effects' uniforms in ComposeUniforms::effectData. Pure: no device,
// so a test reads the generated text.
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace visutwin::canvas
{
    class CameraFrameEffect;

    struct ComposeEffectsBuild
    {
        /// The generated compose-effects.slang; empty when no effect is composed, which means
        /// the file of the tree (and the bundled compose program).
        std::string source;

        /// The effects composed, in the order they apply within each slot.
        std::vector<const CameraFrameEffect*> effects;

        /// Each composed effect's debug views, in order: view i is debug mode
        /// kComposeDebugFirstEffect + i.
        std::vector<std::string> debugViews;

        /// Whether a composed effect requires the scene depth (getLinearScreenDepth).
        bool requiresDepth = false;

        /// Where each uniform's rows start in effectData.
        struct Placement
        {
            const CameraFrameEffect* effect = nullptr;
            size_t uniform = 0;   // index into effect->uniforms()
            int row = 0;
        };
        std::vector<Placement> placements;

        /// Effects left out, and why (out of uniform rows, an empty chunk).
        std::vector<std::string> errors;
    };

    /// The composition of `effects` (the camera's, in registration order): those that are
    /// active and have a slot and a chunk, and an active effect with no slot whose debug views
    /// include `debugView`, the one shown (it is called at no slot).
    ComposeEffectsBuild buildComposeEffects(const std::vector<std::shared_ptr<CameraFrameEffect>>& effects,
        const std::string& debugView = {});

    /// Writes the composed effects' current uniform values into `rows` (kComposeEffectDataRows
    /// float4s) at the build's placements.
    void packComposeEffectData(const ComposeEffectsBuild& build, float (*rows)[4]);

    /// The debug mode for a view name: a built-in view (scene, depth, bloom, ssao, dofcoc,
    /// dofblur, vignette) or one of the build's effect views; kComposeDebugNone for an empty
    /// name, and kComposeDebugMissing for one nothing provides.
    uint32_t composeDebugMode(const std::string& view, const ComposeEffectsBuild& build);
}
