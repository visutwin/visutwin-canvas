// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#pragma once

#include <memory>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/renderPass.h"
#include "scene/composition/renderAction.h"
#include "scene/constants.h"
#include "cameraFrameEffect.h"
#include "renderPassConstants.h"

namespace visutwin::canvas
{
    class LayerComposition;
    class Renderer;
    class Scene;
    class RenderPassForward;
    class RenderPassPrepass;
    class RenderPassColorGrab;
    class RenderPassDepthGrab;
    class RenderPassSsao;
    class RenderPassVolumetricFog;
    class RenderPassVolumetricFogCombine;
    class RenderPassDownsample;
    class RenderPassBloom;
    class RenderPassDof;
    class RenderPassCompose;

    // The colour settings (ComposeColorSettings) are copied from the camera's
    // RenderingSettings as one block and handed to the compose pass as one block.
    struct CameraFrameOptions : ComposeColorSettings
    {
        std::vector<PixelFormat> formats;
        bool stencil = false;
        int samples = 1;
        bool sceneColorMap = false;
        // The post-opaque depth COPY screen-space reflections march against.
        // Requested through CameraComponent::requestSceneDepthMap like the colour grab.
        bool sceneDepthMap = false;
        int lastGrabLayerId = LAYERID_SKYBOX;
        bool lastGrabLayerIsTransparent = false;
        int lastSceneLayerId = LAYERID_IMMEDIATE;
        bool lastSceneLayerIsTransparent = true;
        bool taaEnabled = false;
        // Supersampling factor for the scene render target: >1 renders the scene
        // larger than the camera's output and lets the compose blit filter it back
        // down (SSAA), <1 renders smaller and upscales. 1.0 is native.
        float renderTargetScale = 1.0f;
        bool bloomEnabled = false;
        float bloomIntensity = 0.01f;
        int bloomBlurLevel = 16;
        float bloomThreshold = 0.0f;
        bool bloomHighQuality = false;
        float sharpness = 0.0f;
        std::string_view ssaoType = SSAOTYPE_NONE;
        bool ssaoBlurEnabled = true;
        // "Something samples the scene depth": set for every depth consumer by
        // sanitizeOptions. Whether a prepass actually RENDERS for it is prepassRenders().
        bool prepassEnabled = false;
        bool dofEnabled = false;
        bool dofNearBlur = false;
        bool dofHighQuality = true;
        // Volumetric fog samples scene depth after the scene pass, so it is a
        // depth consumer for the prepass rule, and its combine pass reloads the
        // scene target, so that target cannot be transient.
        bool fogEnabled = false;

        // The camera's active frame effects (CameraComponent::addFrameEffect): each one's
        // identity, required resources and, when it owns passes, build key — a change
        // rebuilds the frame — and what they require of it.
        std::string effectsKey;
        bool effectDepth = false;           // FrameResource::Depth (or PrepassDepth)
        bool effectPrepassDepth = false;    // FrameResource::PrepassDepth: the prepass renders
        bool effectSceneColorHalf = false;
        bool effectSceneTarget = false;
    };

    class RenderPassCameraFrame : public RenderPass
    {
    public:
        // Index of the last action in `actions` a pass stopping at (layer, transparent)
        // renders, searching from `fromIndex`: fromIndex - 1 when it renders none, or
        // kStopLayerNotInComposition. A stop layer with no action of its own (disabled)
        // is placed by its POSITION in the composition. Static so tests/cameraFrameStopTests.cpp can drive it on a composition.
        static constexpr int kStopLayerNotInComposition = -1000000;
        static int findActionIndex(const std::vector<RenderAction*>& actions, LayerComposition* composition,
            int targetLayerId, bool targetTransparent, int fromIndex);

        // Whether sanitized options make a depth prepass RENDER: under MSAA for any depth
        // consumer, single-sampled only for lighting-mode SSAO (see the definition).
        // Static for the same test.
        static bool prepassRenders(const CameraFrameOptions& options);

        RenderPassCameraFrame(const std::shared_ptr<GraphicsDevice>& device, LayerComposition* layerComposition, Scene* scene,
            Renderer* renderer, const std::vector<RenderAction*>& sourceActions, CameraComponent* cameraComponent,
            const std::shared_ptr<RenderTarget>& targetRenderTarget);
        ~RenderPassCameraFrame() override;

        void destroy();
        void reset();
        void update(const CameraFrameOptions& options);
        bool needsReset(const CameraFrameOptions& options) const;
        CameraFrameOptions sanitizeOptions(const CameraFrameOptions& options) const;

        /**
         * Overlays the camera component's TAA/DOF/SSAO/rendering settings onto `options`.
         * Used by both the constructor and the per-frame rebuild so no setting is silently
         * dropped after the first frame.
         */
        void applyCameraSettings(CameraFrameOptions& options) const;

        // DEVIATION: upstream RenderPassCameraFrame uses addLayers() to self-build
        // render actions from the LayerComposition.  This version receives sourceActions
        // from ForwardRenderer.  updateSourceActions() refreshes per-frame data (source
        // actions, composition, scene, renderer) without recreating GPU textures.
        void updateSourceActions(const std::vector<RenderAction*>& sourceActions,
            LayerComposition* layerComposition, Scene* scene, Renderer* renderer,
            const std::shared_ptr<RenderTarget>& targetRenderTarget);

        void setRenderTargetScale(float value);
        float renderTargetScale() const { return _renderTargetScale; }

        void frameUpdate() const override;

        /// Publishes this frame's scene depth, depth grab and lighting-mode SSAO texture
        /// on the device. Run by the frame's first pass when it renders, not by
        /// frameUpdate, which runs for every camera while the graph is built.
        void publishSceneInputs() const;

        /// The passes the frame's effects added, per stage, as built at the last rebuild.
        const CameraFrameEffectPasses& effectPasses() const { return _effectStagePasses; }

    private:
        void setupRenderPasses(const CameraFrameOptions& options);
        void setupEffectPasses(const CameraFrameOptions& options);
        void destroyEffectPasses();
        void createPasses(const CameraFrameOptions& options);
        void updateCameraUseFlags();
        void setupScenePrepass(const CameraFrameOptions& options);
        void createPrepassRenderTarget() const;
        // The size of what the frame renders into: the camera's target, or the device.
        std::pair<int, int> outputSize() const;
        struct ScenePassesInfo
        {
            int lastAddedIndex = -1;
            bool clearRenderTarget = true;
        };
        ScenePassesInfo setupScenePass(const CameraFrameOptions& options);
        void setupSsaoPass(const CameraFrameOptions& options);
        void setupVolumetricFogPass(const CameraFrameOptions& options);
        void setupSceneHalfPass(const CameraFrameOptions& options, Texture* sourceTexture);
        void setupBloomPass(const CameraFrameOptions& options, Texture* inputTexture);
        void setupDofPass(const CameraFrameOptions& options, Texture* inputTexture, Texture* inputTextureHalf);
        void applyDisplaySettings() const;
        void setupComposePass(const CameraFrameOptions& options);
        void setupAfterPass(const CameraFrameOptions& options, const ScenePassesInfo& scenePassesInfo);

        std::vector<std::shared_ptr<RenderPass>> collectPasses() const;
        int appendActionsToPass(const std::shared_ptr<RenderPassForward>& pass, int fromIndex, int toIndex,
            const std::shared_ptr<RenderTarget>& target, bool firstLayerClears = true);
        int findActionIndex(int targetLayerId, bool targetTransparent, int fromIndex) const;
        static std::shared_ptr<RenderAction> cloneActionWithTarget(const RenderAction* source,
            const std::shared_ptr<RenderTarget>& renderTarget);

        CameraFrameOptions _options;
        LayerComposition* _layerComposition = nullptr;
        Scene* _scene = nullptr;
        Renderer* _renderer = nullptr;
        CameraComponent* _cameraComponent = nullptr;
        std::shared_ptr<RenderTarget> _targetRenderTarget;
        std::vector<RenderAction*> _sourceActions;

        PixelFormat _hdrFormat = PixelFormat::PIXELFORMAT_RGBA8;
        bool _bloomEnabled = false;
        bool _sceneHalfEnabled = false;
        float _renderTargetScale = 1.0f;
        std::shared_ptr<RenderPassOptions> _sceneOptions;
        bool _needsReset = false;

        std::shared_ptr<RenderTarget> _sceneRenderTarget;
        // Depth-only view of the scene target's depth attachment, for the prepass.
        // Its own target rather than the scene one, because the scene pass clears and
        // re-renders the same depth straight afterwards, and because with MSAA the
        // scene target's depth is a multisampled twin while THIS is the single-sampled
        // texture every later pass samples. Mutable because a resize of the shared
        // depth texture has to rebuild it from frameUpdate, which is const.
        mutable std::shared_ptr<RenderTarget> _prepassRenderTarget;
        std::shared_ptr<Texture> _sceneTexture;
        std::shared_ptr<Texture> _sceneDepthTexture;
        std::shared_ptr<RenderTarget> _sceneHalfRenderTarget;
        std::shared_ptr<Texture> _sceneTextureHalf;

        // Runs ahead of every other pass of the frame and publishes its inputs then, not
        // in frameUpdate: frameUpdate runs for every camera while the graph is BUILT, so a
        // publish there hands each camera's passes the LAST camera's textures.
        std::shared_ptr<RenderPass> _publishPass;
        std::shared_ptr<RenderPassPrepass> _prePass;
        std::shared_ptr<RenderPassForward> _scenePass;
        std::shared_ptr<RenderPassColorGrab> _colorGrabPass;
        std::shared_ptr<RenderPassDepthGrab> _depthGrabPass;
        std::shared_ptr<RenderPassForward> _scenePassTransparent;
        std::shared_ptr<RenderPassSsao> _ssaoPass;
        std::shared_ptr<RenderPassVolumetricFog> _volumetricFogPass;
        std::shared_ptr<RenderPassVolumetricFogCombine> _volumetricFogCombinePass;
        // The engine's updatedTime() when this frame last rendered; negative before the first.
        mutable double _lastUpdatedTime = -1.0;
        std::shared_ptr<RenderPassDownsample> _scenePassHalf;
        std::shared_ptr<RenderPassBloom> _bloomPass;
        std::shared_ptr<RenderPassDof> _dofPass;
        std::shared_ptr<RenderPassCompose> _composePass;
        std::shared_ptr<RenderPassForward> _afterPass;

        // The frame effects whose passes were created at the last rebuild (destroyPasses goes
        // to these, even after the camera let them go), and their passes per stage. update /
        // frameUpdate go to the camera's active effects as they are each frame.
        std::vector<std::shared_ptr<CameraFrameEffect>> _passEffects;
        CameraFrameEffectPasses _effectStagePasses;
        // FrameResource::SceneTarget single-sampled: a colour-only target over the scene
        // texture, so an effect pass may sample the scene depth while drawing into the
        // scene. It shares the texture the scene target resizes, so its attachments are
        // rebuilt when that size moves (the recorded size, since its own width() reads the
        // shared texture).
        std::shared_ptr<RenderTarget> _sceneColorTarget;
        mutable int _sceneColorTargetWidth = 0;
        mutable int _sceneColorTargetHeight = 0;

        mutable Texture* _sceneTextureResolved = nullptr;
        std::vector<std::shared_ptr<RenderAction>> _ownedActions;
    };
}
