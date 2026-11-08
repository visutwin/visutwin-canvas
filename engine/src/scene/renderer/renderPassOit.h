// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.11.2026
//
// Order-independent transparency, weighted blended (McGuire and Bavoil, "Weighted Blended
// Order-Independent Transparency", JCGT 2013), for a layer that asks for it
// (Layer::setOrderIndependentTransparency). A pass with no target of its own whose two
// before-passes do the work:
//   - the ACCUMULATION, a forward pass drawing the layer's transparent sublayer into two
//     attachments, cleared to zero: RGBA16F holding the sum of colour * alpha * weight and of
//     alpha * weight, and RGBA8 holding the coverage 1 - prod(1 - alpha) (a premultiplied "over"
//     of alpha, which also starts at zero). The draws blend additively whatever their
//     materials say, and each fragment tests itself against the camera's scene depth map in
//     the shader (forwardFragmentOit): the targets have no depth attachment, so the pass works
//     the same over the back buffer, a camera's own target and a camera frame's scene target.
//   - the COMPOSITE, a quad over the camera's target blending the weighted average colour by
//     the coverage (oit-composite.slang).
// The weight falls off with the fragment's view depth (the paper's equation 7), so a nearer
// surface dominates where several overlap; the result is an approximation that needs no sort.
//
#pragma once

#include <memory>
#include <vector>

#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class BlendState;
    class CameraComponent;
    class LayerComposition;
    class Renderer;
    class RenderPassForward;
    class RenderPassShaderQuad;
    class RenderTarget;
    class Scene;
    class Texture;
    struct RenderAction;

    /// Whether `action` is drawn by a RenderPassOit: the transparent sublayer of a layer with
    /// order-independent transparency, rendered by a camera's layers (not its own passes).
    bool isOrderIndependentAction(const RenderAction& action);

    class RenderPassOit : public RenderPass
    {
    public:
        RenderPassOit(const std::shared_ptr<GraphicsDevice>& device, LayerComposition* layerComposition,
            Scene* scene, Renderer* renderer, CameraComponent* camera);
        ~RenderPassOit() override;

        /// What the pass draws this frame: `actions` (transparent sublayers of layers with
        /// order-independent transparency, all of `camera`), composited over `target` (null:
        /// the back buffer) of `width` x `height` pixels. `hdr` for a camera frame's scene
        /// target (linear output). With `cameraEvents` the accumulation fires the camera's
        /// prerender / postrender when an action is the camera's first / last of the frame (a
        /// camera frame marks its own passes instead). Call before the pass is added to the frame
        /// graph: the graph takes the before-passes as it adds the pass.
        void setup(const std::vector<RenderAction*>& actions, const std::shared_ptr<RenderTarget>& target,
            int width, int height, bool hdr, bool cameraEvents);

        /// The composition the accumulation checks its actions' layers against.
        void setLayerComposition(LayerComposition* layerComposition) { _layerComposition = layerComposition; }

        /// The composite pass (the last to write the camera's target), once set up.
        RenderPass* compositePass() const;

        Texture* accumulationTexture() const { return _accumulation.get(); }
        Texture* coverageTexture() const { return _coverage.get(); }

        /// The accumulation's blend: attachment 0 adds (ONE, ONE), attachment 1 is the
        /// premultiplied "over" of the coverage (ONE, ONE_MINUS_SRC_ALPHA). Shared.
        static const std::shared_ptr<BlendState>& accumulationBlendState();
        /// The composite's blend: the average colour over the target by the coverage.
        static const std::shared_ptr<BlendState>& compositeBlendState();

    private:
        void ensureTargets(int width, int height);

        LayerComposition* _layerComposition;
        Scene* _scene;
        Renderer* _renderer;
        CameraComponent* _camera;
        std::shared_ptr<Texture> _accumulation;
        std::shared_ptr<Texture> _coverage;
        std::shared_ptr<RenderTarget> _target;
        std::shared_ptr<RenderPassForward> _accumulatePass;
        std::shared_ptr<RenderPassShaderQuad> _compositePass;
        std::vector<std::shared_ptr<RenderAction>> _ownedActions;
    };
}
