// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2026
//
// An application's extension of a camera frame: an effect registered with a camera
// (CameraComponent::addFrameEffect) that adds render passes of its own at fixed stages of
// the frame and asks the frame for the resources those passes read.
//
// The stages, in frame order:
//   preScene     after the depth prepass, before the scene renders: output the scene's
//                materials read as they render;
//   postOpaque   after the opaque layers, the skybox and the scene-colour copy, before
//                the transparent layers: render into the scene under the transparents;
//   postScene    after the whole scene (and the volumetric fog), before TAA;
//   postTemporal after TAA, the half-resolution copy, depth of field and bloom, before
//                the composition.
// Passes of one stage run in the order their effects were registered, after the frame's
// own passes at that point.
//
// Requiring a resource turns on what produces it: the scene-colour copy, the depth
// prepass, the half-resolution copy. The resources stay valid for the life of the passes
// created from them; the frame recreates the passes whenever the set of active effects,
// any one's required resources or its buildKey() changes.
//
// DEVIATION: an effect is registered on the camera component, which co-owns it, because
// a camera frame here is created by the renderer for the camera rather than by the
// application. An effect contributes passes only; it cannot add code to the composition.
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace visutwin::canvas
{
    class CameraComponent;
    class GraphicsDevice;
    class RenderPass;
    class RenderTarget;
    class Texture;

    /// A resource of the camera frame an effect's passes read.
    enum class FrameResource
    {
        /// The scene depth: standard [0, 1] device depth (near 0), linearised with the
        /// camera's near and far clip (`quadLinearDepth` in the vtquad module). Rendered by
        /// the scene pass, or by the prepass under MSAA; valid from postOpaque on.
        Depth,
        /// The same texture, guaranteed to hold the opaque depth BEFORE the scene renders:
        /// requiring it renders the depth prepass. What a preScene pass reads.
        PrepassDepth,
        /// The scene colour at half resolution, after TAA. Valid at postTemporal.
        SceneColorHalf,
        /// The copy of the opaque scene colour (linear HDR) taken before the transparent
        /// layers. Valid from postOpaque on.
        SceneColorGrab,
        /// A render target onto the scene colour, for a pass that draws into the scene.
        SceneTarget,
    };

    /// What the frame hands an effect to create its passes from.
    struct CameraFrameEffectResources
    {
        std::shared_ptr<GraphicsDevice> device;
        CameraComponent* camera = nullptr;

        /// Depth / PrepassDepth: the scene depth texture. Single-sampled it is the depth
        /// attachment of the frame's scene target, which is why sceneTarget is a colour-only
        /// target then: a pass drawing into sceneTarget may sample this freely.
        Texture* depth = nullptr;

        /// SceneColorHalf.
        Texture* sceneColorHalf = nullptr;

        /// SceneColorGrab: the copy's texture, asked for when the pass draws. The copy is
        /// reallocated when the scene resizes, so keep the function, not its answer.
        std::function<Texture*()> sceneColorGrab;

        /// SceneTarget: colour only when single-sampled (the scene texture alone), the
        /// multisampled scene target under MSAA, whose resolve happens at the end of the
        /// frame's last scene pass. Either way a pass loads what is there and its result
        /// stays in the scene.
        std::shared_ptr<RenderTarget> sceneTarget;
    };

    /// The passes an effect adds, one list per stage.
    struct CameraFrameEffectPasses
    {
        std::vector<std::shared_ptr<RenderPass>> preScene;
        std::vector<std::shared_ptr<RenderPass>> postOpaque;
        std::vector<std::shared_ptr<RenderPass>> postScene;
        std::vector<std::shared_ptr<RenderPass>> postTemporal;
    };

    /// Per-frame values an effect's frameUpdate() gets.
    struct CameraFrameEffectContext
    {
        /// The scene colour this frame: TAA's resolved history under TAA.
        Texture* sceneTexture = nullptr;
        /// The scene's size in pixels this frame.
        int sceneWidth = 0;
        int sceneHeight = 0;
    };

    class CameraFrameEffect
    {
    public:
        /// `id` names the effect on its camera, where ids are unique. `requiredResources` is what
        /// requiredResources() returns unless a subclass decides it from its parameters.
        explicit CameraFrameEffect(std::string id, std::vector<FrameResource> requiredResources = {});
        virtual ~CameraFrameEffect() = default;

        CameraFrameEffect(const CameraFrameEffect&) = delete;
        CameraFrameEffect& operator=(const CameraFrameEffect&) = delete;

        [[nodiscard]] const std::string& id() const { return _id; }

        /// A disabled effect takes no part: its passes are destroyed at the next rebuild and
        /// its resources are no longer produced for it.
        [[nodiscard]] bool enabled() const { return _enabled; }
        void setEnabled(bool value) { _enabled = value; }

        /// Whether the effect takes part. The enabled flag by default; a subclass can also
        /// say no for parameters that make it do nothing.
        [[nodiscard]] virtual bool active() const { return _enabled; }

        /// The resources the effect's passes read.
        [[nodiscard]] virtual std::vector<FrameResource> requiredResources() const { return _requiredResources; }

        /// True for an effect that adds passes (createPasses / destroyPasses).
        [[nodiscard]] virtual bool ownsPasses() const { return false; }

        /// Creates the effect's passes from the frame's resources and adds each to its stage.
        /// Called when the frame (re)builds, for an active effect owning passes.
        virtual void createPasses(const CameraFrameEffectResources& /*resources*/, CameraFrameEffectPasses& /*passes*/) {}

        /// Releases the passes createPasses made; called before a rebuild and when the frame
        /// goes away. The frame drops its own references to them as well.
        virtual void destroyPasses() {}

        /// Whatever else, beyond requiredResources(), the passes are built from: a change rebuilds
        /// them. Settings a pass reads every frame do not belong here.
        [[nodiscard]] virtual std::string buildKey() const { return {}; }

        /// Called every frame for an active effect once its passes exist, before any of them
        /// renders: where the effect's settings are handed to its passes.
        virtual void update() {}

        /// Called every frame after update(), with the values of this frame.
        virtual void frameUpdate(const CameraFrameEffectContext& /*context*/) {}

        /// A process-wide serial: what the frame identifies an effect instance by, so that
        /// a new instance with the id of a removed one still rebuilds the passes.
        [[nodiscard]] uint64_t serial() const { return _serial; }

    private:
        std::string _id;
        std::vector<FrameResource> _requiredResources;
        bool _enabled = true;
        uint64_t _serial = 0;
    };
}
