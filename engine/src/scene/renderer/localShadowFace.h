// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <memory>

#include "core/math/vector4.h"

namespace visutwin::canvas
{
    class Camera;
    class GraphicsDevice;
    class Light;
    class ProgramLibrary;
    class Shader;
    struct DepthOnlyShaders;

    /**
     * The state every local shadow pass binds before drawing: the depth-only
     * shadow shader, plain blend and depth state, and the light's hardware polygon
     * offset (skipped for an omni light, whose relative bias is applied in the
     * forward shader, and for PCSS, which biases in the shader too). Returns false
     * — and says why, once — when no shadow shader exists for the device.
     */
    bool bindLocalShadowState(GraphicsDevice* device, ProgramLibrary* programLibrary,
        const Light* light, DepthOnlyShaders& shaders);

    /**
     * Draws face `face` of `light`'s shadow into whatever target and viewport are
     * bound: the pre-classified caster list for an omni light, a culled sweep of
     * the scene for a spot. Shared by the per-face non-clustered pass and the
     * single clustered atlas pass, so the two cannot drift.
     */
    void drawLocalShadowFace(GraphicsDevice* device, ProgramLibrary* programLibrary,
        DepthOnlyShaders& shaders, Light* light, int face, Camera* shadowCamera);

    /**
     * Clears the depth of `rect` (pixels) in the bound depth-only target to 1.0,
     * inside the render pass. A load action can only clear the whole attachment,
     * and the clustered atlas holds one-shot shadows that must survive while a
     * neighbouring slot re-renders, so a face's rect is cleared by drawing a
     * fullscreen triangle at depth 1 under a viewport and scissor with the depth
     * test set to ALWAYS — which is how upstream's WebGPU backend clears a
     * viewport too. Restores the viewport and scissor; the caller re-binds its
     * own depth state and shader afterwards.
     */
    void clearDepthRect(GraphicsDevice* device, const Vector4& rect);
}
