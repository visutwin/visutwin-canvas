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
//   postScene    after the whole scene (and the volumetric fog), before it is resolved;
//   resolve      passes RESOLVING the scene: they read the scene colour and render a new
//                one, which the rest of the frame reads in its place (resolveScene). The
//                temporal anti-aliasing is one, the camera's built-in TaaEffect, and runs
//                first;
//   postResolve  after the resolved scene's half-resolution copy, depth of field and
//                bloom, before the composition.
// Passes of one stage run in the order their effects were registered, after the frame's
// own passes at that point.
//
// Requiring a resource turns on what produces it: the scene-colour copy, the depth
// prepass, the half-resolution copy. The resources stay valid for the life of the passes
// created from them; the frame recreates the passes whenever the set of active effects,
// any one's required resources or its buildKey() changes.
//
// An effect can also add code to the composition, the frame's single compose pass, at one
// of four slots (ComposeSlot), with no pass of its own: a Slang chunk defining
// `apply<Id>` (float3 (float3 color, float2 uv) at Hdr / Ldr / Output, float4 (float4 color,
// float2 uv) at Scene), called after the frame's own effects at that slot and in
// registration order. The chunk reads `sceneTexture` (the scene, a Sampler2D),
// `sceneTextureSize` (width, height, 1 / width, 1 / height), `getLinearScreenDepth(uv)` when
// the effect requires FrameResource::Depth, and its uniforms by name. The frame declares
// the uniforms (setUniform), so the chunk declares none; setDefine adds a #define, and
// `<ID>` (the id in upper snake case) is defined while the effect is active. A view named in
// `debugViews` is a `float3 debug<Name>()` in the chunk, shown through
// RenderingSettings::composeDebug; it reads the pixel being shown as `composeDebugUv`. An
// effect with no slot may still provide debug views: its chunk joins the composition only
// while one of them is shown. The compose shader is compiled at run time for each
// distinct set of chunks, defines and uniform names (a uniform's VALUE costs nothing).
//
// DEVIATIONS: an effect is registered on the camera component, which co-owns it, because
// a camera frame here is created by the renderer for the camera rather than by the
// application; the chunk is Slang, declares no uniforms, cannot be overridden through
// ShaderChunks, and takes no texture uniform.
//
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace visutwin::canvas
{
    class CameraComponent;
    struct Color;
    class GraphicsDevice;
    struct Matrix4;
    struct Vector2;
    struct Vector3;
    struct Vector4;
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
        /// The resolved scene colour at half resolution. Valid at postResolve.
        SceneColorHalf,
        /// The copy of the opaque scene colour (linear HDR) taken before the transparent
        /// layers. Valid from postOpaque on.
        SceneColorGrab,
        /// A render target onto the scene colour, for a pass that draws into the scene.
        SceneTarget,
    };

    /// Where in the compose pass an effect's chunk is called. In chain order: Scene right after
    /// the scene sample (after CAS and fringing), Hdr in linear scene-referred colour before
    /// tone mapping (after grading), Ldr after tone mapping, the colour LUT and the vignette,
    /// before gamma, Output after gamma.
    enum class ComposeSlot
    {
        None,
        Scene,
        Hdr,
        Ldr,
        Output,
    };

    /// An effect's declarations; fixed for its life.
    struct CameraFrameEffectOptions
    {
        ComposeSlot slot = ComposeSlot::None;
        /// The compose chunk, Slang; read only while the slot is not None.
        std::string slang;
        /// Debug views the chunk provides, each a `float3 debug<Name>()`.
        std::vector<std::string> debugViews;
        /// The function called at the slot; empty for `apply<Id>`.
        std::string entryPoint;
        std::vector<FrameResource> requiredResources;
    };

    /// A uniform an effect set: its float4 rows (1, or 4 for a matrix) and how many lanes the
    /// shader reads (1-4; 16 for a matrix).
    struct ComposeEffectUniform
    {
        std::string name;
        int components = 1;
        std::array<float, 16> value{};
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

        /// The scene render target's colour, which the first resolving effect reads (always
        /// set). What a resolving pass sizes itself from before its first frame; each frame
        /// it gets the texture to read in resolveScene.
        Texture* sceneColor = nullptr;

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
        std::vector<std::shared_ptr<RenderPass>> resolve;
        std::vector<std::shared_ptr<RenderPass>> postResolve;
    };

    /// The scene being resolved, handed to each effect owning passes in turn (resolveScene):
    /// the scene colour the effects before it resolved, which a resolving effect replaces with
    /// what its passes render to this frame. Starts as the scene render target's colour.
    struct CameraFrameEffectScene
    {
        Texture* texture = nullptr;
        /// The scene colour's size this frame; its texture may still have last frame's size.
        /// An effect resolving to another resolution sets these too.
        int width = 0;
        int height = 0;
        /// The size of what the frame composes into (the camera's render target, or the back
        /// buffer): what an effect resolving to the full resolution (TAA upscaling) resolves to.
        int outputWidth = 0;
        int outputHeight = 0;
    };

    /// Per-frame values an effect's frameUpdate() gets.
    struct CameraFrameEffectContext
    {
        /// The resolved scene colour this frame (TAA's history, which alternates between two
        /// textures, under TAA).
        Texture* sceneTexture = nullptr;
        /// The resolved scene's size in pixels this frame.
        int sceneWidth = 0;
        int sceneHeight = 0;
        /// The size the scene renders at this frame: the scene render target's, which the
        /// passes drawing into the scene have. Differs from the scene size only when a
        /// resolving effect changes the resolution.
        int renderWidth = 0;
        int renderHeight = 0;
        /// The exposure the composition displays the scene with: the camera's, or 1 under tone
        /// mapping NONE, which applies none. What a pass judging or blending the scene as it is
        /// SEEN scales the linear scene by.
        float displayExposure = 1.0f;
        /// The time the world advanced by since the camera frame last rendered, in seconds:
        /// the engine's update steps since (clamped and time-scaled, as scripts get them). 0
        /// for a frame rendered again without an update, while the world is paused, and on
        /// the camera frame's first frame. Anything an effect advances over time advances by
        /// this, so it pauses with the world.
        float dt = 0.0f;
    };

    class CameraFrameEffect
    {
    public:
        /// `id` names the effect on its camera, where ids are unique. `requiredResources` is what
        /// requiredResources() returns unless a subclass decides it from its parameters.
        explicit CameraFrameEffect(std::string id, std::vector<FrameResource> requiredResources = {});
        CameraFrameEffect(std::string id, CameraFrameEffectOptions options);
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

        /// Whether the effect renders the camera's sky while it is active. The camera then does
        /// not draw the scene's sky, which the effect replaces.
        [[nodiscard]] virtual bool rendersSky() const { return false; }

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

        /// Resolves the scene, for an effect with passes at the resolve stage: called every
        /// frame while its passes take part, after update() and before any frameUpdate(), in
        /// registration order (the built-in TAA first). Hand the passes `scene.texture` and
        /// replace it with what they render to this frame; the effects after it, the
        /// half-resolution copy, depth of field, bloom and the composition read that. The
        /// default leaves the scene unchanged.
        virtual void resolveScene(CameraFrameEffectScene& /*scene*/) {}

        /// The scale the effect applies to its camera's exposure this frame, for an effect
        /// adapting the exposure to what the camera sees: read every frame after frameUpdate(),
        /// and the camera renders with the product over its active effects (Camera::
        /// exposureScale), back at its own exposure once none asks.
        [[nodiscard]] virtual float cameraExposureScale() const { return 1.0f; }

        /// The camera the effect is registered with (CameraComponent::addFrameEffect), or null.
        [[nodiscard]] CameraComponent* camera() const { return _camera; }

        /// The camera jitter the effect needs while it is active, 0-1: the sub-pixel offset of
        /// the projection, different every frame, that an effect accumulating frames relies on.
        /// The camera takes the largest any active effect of its frame asks for.
        [[nodiscard]] virtual float cameraJitter() const { return 0.0f; }

        /// Called every frame after update() and the resolve, with the values of this frame.
        virtual void frameUpdate(const CameraFrameEffectContext& /*context*/) {}

        /// The compose slot, the chunk and its debug views (see the file comment).
        [[nodiscard]] ComposeSlot slot() const { return _slot; }
        [[nodiscard]] const std::string& slang() const { return _slang; }
        [[nodiscard]] const std::vector<std::string>& debugViews() const { return _debugViews; }
        /// The function the composition calls: `apply<Id>` unless given.
        [[nodiscard]] std::string entryPoint() const;
        /// Defined on the compose shader while the effect is active: `colorEnhance` gives
        /// `COLOR_ENHANCE`.
        [[nodiscard]] std::string defineName() const;

        /// Sets a #define for the chunk; an empty value removes it. Kept until changed, so a
        /// fixed define is set once and one depending on a parameter in update(). A change
        /// compiles another compose shader.
        void setDefine(const std::string& name, const std::string& value = "1");
        [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& defines() const { return _defines; }

        /// Sets a uniform the chunk reads by `name`. Set the values in update(), or in
        /// frameUpdate() for one that changes every frame; a new name or another type
        /// compiles another compose shader, a new value does not.
        void setUniform(const std::string& name, float value);
        void setUniform(const std::string& name, const Vector2& value);
        void setUniform(const std::string& name, const Vector3& value);
        void setUniform(const std::string& name, const Vector4& value);
        void setUniform(const std::string& name, const Color& value);   // float4, rgba
        void setUniform(const std::string& name, const Matrix4& value); // float4x4, as the engine's matrices
        [[nodiscard]] const std::vector<ComposeEffectUniform>& uniforms() const { return _uniforms; }

        /// A process-wide serial: what the frame identifies an effect instance by, so that
        /// a new instance with the id of a removed one still rebuilds the passes.
        [[nodiscard]] uint64_t serial() const { return _serial; }

    private:
        friend class CameraComponent;
        ComposeEffectUniform& uniformSlot(const std::string& name, int components);

        CameraComponent* _camera = nullptr;

        std::string _id;
        std::vector<FrameResource> _requiredResources;
        ComposeSlot _slot = ComposeSlot::None;
        std::string _slang;
        std::vector<std::string> _debugViews;
        std::string _entryPoint;
        std::vector<std::pair<std::string, std::string>> _defines;
        std::vector<ComposeEffectUniform> _uniforms;
        bool _enabled = true;
        uint64_t _serial = 0;
    };
}
