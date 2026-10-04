// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.09.2026
//
#pragma once

#include <memory>

#include "core/math/vector4.h"

namespace visutwin::canvas
{
    class Camera;
    class ComponentRegistry;
    class GraphicsDevice;
    class Light;
    class ProgramLibrary;
    class Shader;
    struct DepthOnlyShaders;

    /**
     * The state every local shadow pass binds before drawing: the depth-only
     * shadow shader, plain blend and depth state, and the light's hardware polygon
     * offset (skipped for an omni light, whose relative bias is applied in the
     * forward shader, and for PCSS and VSM, which bias in the shader too). With
     * `vsm` the programs write EVSM moments of the distance to the light (a VSM spot
     * light's own map; the clustered atlas is depth-only). Returns false — and says
     * why, once — when no shadow shader exists for the device.
     */
    bool bindLocalShadowState(GraphicsDevice* device, ProgramLibrary* programLibrary,
        const Light* light, DepthOnlyShaders& shaders, bool vsm = false);

    /// Creates what a local shadow pass draws with before it draws: the depth-only
    /// programs bindLocalShadowState binds and, for the atlas pass, the program
    /// clearDepthRect clears a rect with. For the passes' prepareShaders().
    void prepareLocalShadowShaders(const std::shared_ptr<GraphicsDevice>& device, bool clearsRects,
        bool vsm = false);

    /**
     * Draws face `face` of `light`'s shadow into whatever target and viewport are
     * bound: the caster list the light's cull prepared this frame, or a culled sweep
     * of the scene when there is none. Shared by the per-face non-clustered pass and
     * the single clustered atlas pass, so the two cannot drift.
     */
    void drawLocalShadowFace(GraphicsDevice* device, ProgramLibrary* programLibrary,
        DepthOnlyShaders& shaders, Light* light, int face, Camera* shadowCamera,
        ComponentRegistry* registry);

    /**
     * Clears the depth of `rect` (pixels) in the bound depth-only target to 1.0,
     * inside the render pass. A load action can only clear the whole attachment,
     * and the clustered atlas holds one-shot shadows that must survive while a
     * neighbouring slot re-renders, so a face's rect is cleared by drawing a
     * fullscreen triangle at depth 1 under a viewport and scissor with the depth
     * test set to ALWAYS. Restores the viewport and scissor; the caller re-binds its
     * own depth state and shader afterwards.
     */
    void clearDepthRect(GraphicsDevice* device, const Vector4& rect);

    /**
     * Clears the depth of the bound target, within the current viewport, to 1.0 INSIDE a
     * render pass that also has colour attachments, leaving the colour untouched: the same
     * depth-1 triangle under ALWAYS, with every colour write masked off. A render action
     * that is not the first of its pass is cleared in the middle of the pass, which is how a layer
     * with `clearDepthBuffer` (a gizmo layer, the layers example's front layer) draws over
     * everything before it; a load action can only clear at the start of a pass.
     */
    void clearDepthInPass(GraphicsDevice* device);

    /// Creates the shader clearDepthInPass draws with, for a pass's prepareShaders().
    void prepareClearDepthShader(GraphicsDevice* device);
}
