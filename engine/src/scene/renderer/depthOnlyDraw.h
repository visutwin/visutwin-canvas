// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <memory>

#include "core/math/matrix4.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class MeshInstance;
    class ProgramLibrary;
    class Shader;

    /**
     * The depth-only shader variants a pass may need, fetched lazily on first use.
     * One of these lives for the duration of a pass's execute(): the shadow programs
     * are cached by the ProgramLibrary, but a caster that needs no opacity frontend
     * shares the pass's own variant so the common case costs one library call rather
     * than one per mesh.
     */
    struct DepthOnlyShaders
    {
        // The variant the pass binds before the loop and every branch restores.
        std::shared_ptr<Shader> plain;
        std::shared_ptr<Shader> dynamicBatch;
        std::shared_ptr<Shader> skinned;
        std::shared_ptr<Shader> morphed;
        std::shared_ptr<Shader> skinnedMorphed;
        std::shared_ptr<Shader> instanced;
        std::shared_ptr<Shader> instancedColor;
    };

    /**
     * Draws one mesh instance into a depth-only target with `viewProjection`.
     *
     * Every pass that writes depth and nothing else does the same work: pick the
     * shadow program for the caster's deformation path (plain, hardware-instanced,
     * dynamic batch, skinned and/or morphed), bind what that path needs — the
     * instance buffer at slot 5, the bone or batch palette, the morph deltas — bind
     * the caster's material only when its OPACITY decides the depth it writes, draw,
     * and put the pass's own shader back. Three passes need it: the two shadow
     * passes and the depth prepass, and the differences between them are the camera
     * and the caster list, not the draw.
     *
     * The caller is responsible for the pass state the draw does not own: blend and
     * depth state, depth bias, viewport, and the filtering that decides which mesh
     * instances reach here at all — a shadow pass and a prepass disagree about that
     * (castShadow versus depth write), which is why it is not folded in.
     */
    void drawDepthOnly(GraphicsDevice* device, ProgramLibrary* programLibrary,
        MeshInstance* meshInstance, const Matrix4& viewProjection, DepthOnlyShaders& shaders);
}
