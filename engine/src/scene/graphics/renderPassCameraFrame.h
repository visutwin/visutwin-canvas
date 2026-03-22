// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/renderPass.h"
#include "scene/composition/renderAction.h"
#include "scene/constants.h"
#include "renderPassConstants.h"

namespace visutwin::canvas
{
    class LayerComposition;
    class Renderer;
    class Scene;
    class RenderPassForward;
    class RenderPassPrepass;
    class RenderPassColorGrab;
    class RenderPassSsao;
    class RenderPassTAA;
    class RenderPassDownsample;
    class RenderPassBloom;
    class RenderPassDof;
    class RenderPassCompose;

    struct CameraFrameOptions
    {
        std::vector<PixelFormat> formats;
        bool stencil = false;
        int samples = 1;
        bool sceneColorMap = false;
        int lastGrabLayerId = LAYERID_SKYBOX;
        bool lastGrabLayerIsTransparent = false;
        int lastSceneLayerId = LAYERID_IMMEDIATE;
        bool lastSceneLayerIsTransparent = true;
        bool taaEnabled = false;
        bool bloomEnabled = false;
        float bloomIntensity = 0.01f;
        float sharpness = 0.0f;
        std::string_view ssaoType = SSAOTYPE_NONE;
        bool ssaoBlurEnabled = true;
        bool prepassEnabled = false;
        bool dofEnabled = false;
        bool dofNearBlur = false;
        bool dofHighQuality = true;
    };

    class RenderPassCameraFrame : public RenderPass
    {
    public:
        RenderPassCameraFrame(const std::shared_ptr<GraphicsDevice>& device, LayerComposition* layerComposition, Scene* scene,
            Renderer* renderer, const std::vector<RenderAction*>& sourceActions, CameraComponent* cameraComponent,
            const std::shared_ptr<RenderTarget>& targetRenderTarget);

        void destroy();
        void reset();
        void update(const CameraFrameOptions& options);
        bool needsReset(const CameraFrameOptions& options) const;
        CameraFrameOptions sanitizeOptions(const CameraFrameOptions& options) const;

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

    private:
        std::shared_ptr<RenderTarget> createRenderTarget(const std::string& name, bool depth, bool stencil, int samples) const;
        void setupRenderPasses(const CameraFrameOptions& options);
        void createPasses(const CameraFrameOptions& options);
        void setupScenePrepass(const CameraFrameOptions& options);
        struct ScenePassesInfo
        {
            int lastAddedIndex = -1;
            bool clearRenderTarget = true;
        };
        ScenePassesInfo setupScenePass(const CameraFrameOptions& options);
        void setupSsaoPass(const CameraFrameOptions& options);
        Texture* setupTaaPass(const CameraFrameOptions& options);
        void setupSceneHalfPass(const CameraFrameOptions& options, Texture* sourceTexture);
        void setupBloomPass(const CameraFrameOptions& options, Texture* inputTexture);
        void setupDofPass(const CameraFrameOptions& options, Texture* inputTexture, Texture* inputTextureHalf);
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
        std::shared_ptr<Texture> _sceneTexture;
        std::shared_ptr<Texture> _sceneDepthTexture;
        std::shared_ptr<RenderTarget> _sceneHalfRenderTarget;
        std::shared_ptr<Texture> _sceneTextureHalf;

        std::shared_ptr<RenderPassPrepass> _prePass;
        std::shared_ptr<RenderPassForward> _scenePass;
        std::shared_ptr<RenderPassColorGrab> _colorGrabPass;
        std::shared_ptr<RenderPassForward> _scenePassTransparent;
        std::shared_ptr<RenderPassSsao> _ssaoPass;
        std::shared_ptr<RenderPassTAA> _taaPass;
        std::shared_ptr<RenderPassDownsample> _scenePassHalf;
        std::shared_ptr<RenderPassBloom> _bloomPass;
        std::shared_ptr<RenderPassDof> _dofPass;
        std::shared_ptr<RenderPassCompose> _composePass;
        std::shared_ptr<RenderPassForward> _afterPass;

        mutable Texture* _sceneTextureResolved = nullptr;
        std::vector<std::shared_ptr<RenderAction>> _ownedActions;
    };
}
