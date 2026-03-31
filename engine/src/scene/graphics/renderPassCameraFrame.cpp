// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassCameraFrame.h"

#include <algorithm>
#include <cassert>

#include "renderPassBloom.h"
#include "renderPassColorGrab.h"
#include "renderPassCompose.h"
#include "renderPassDof.h"
#include "renderPassDownsample.h"
#include "renderPassPrepass.h"
#include "renderPassSsao.h"
#include "renderPassTAA.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/constants.h"
#include "scene/renderer/renderPassForward.h"
#include "scene/renderer/renderer.h"

namespace visutwin::canvas
{
    namespace
    {
        const CameraFrameOptions defaultOptions{};

        bool formatsEqual(const std::vector<PixelFormat>& a, const std::vector<PixelFormat>& b)
        {
            return a == b;
        }
    }

    RenderPassCameraFrame::RenderPassCameraFrame(const std::shared_ptr<GraphicsDevice>& device,
        LayerComposition* layerComposition, Scene* scene, Renderer* renderer, const std::vector<RenderAction*>& sourceActions,
        CameraComponent* cameraComponent, const std::shared_ptr<RenderTarget>& targetRenderTarget)
        : RenderPass(device), _layerComposition(layerComposition), _scene(scene), _renderer(renderer),
          _cameraComponent(cameraComponent), _targetRenderTarget(targetRenderTarget), _sourceActions(sourceActions)
    {
        // Do NOT call init(nullptr) here. CameraFrame is a container for its
        // before-passes (prepass, scene, TAA, compose, after). If init(nullptr)
        // is called, the CameraFrame's own render() opens a back-buffer render
        // pass (with an empty execute()) that may clear or overwrite the compose
        // output. Leaving _renderTargetInitialized = false causes render() to
        // skip the start/execute/end sequence, which is the correct behavior.

        CameraFrameOptions options = defaultOptions;
        if (_cameraComponent) {
            const auto& taa = _cameraComponent->taa();
            const auto& dof = _cameraComponent->dof();
            const auto& ssao = _cameraComponent->ssao();
            const auto& rendering = _cameraComponent->rendering();
            options.taaEnabled = taa.enabled;
            options.dofEnabled = dof.enabled;
            options.dofNearBlur = dof.nearBlur;
            options.dofHighQuality = dof.highQuality;
            if (ssao.enabled) {
                options.ssaoType = (ssao.type == SSAOTYPE_LIGHTING) ? SSAOTYPE_LIGHTING : SSAOTYPE_COMBINE;
            } else {
                options.ssaoType = SSAOTYPE_NONE;
            }
            options.ssaoBlurEnabled = ssao.blurEnabled;
            options.bloomEnabled = rendering.bloomIntensity > 0.0f;
            options.bloomIntensity = rendering.bloomIntensity;
            options.sharpness = rendering.sharpness;
            options.vignetteEnabled = rendering.vignetteEnabled;
            options.vignetteInner = rendering.vignetteInner;
            options.vignetteOuter = rendering.vignetteOuter;
            options.vignetteCurvature = rendering.vignetteCurvature;
            options.vignetteIntensity = rendering.vignetteIntensity;
        }

        _options = sanitizeOptions(options);
        setupRenderPasses(_options);
    }

    void RenderPassCameraFrame::destroy()
    {
        // Clear device-side raw texture pointers that reference textures owned by this
        // CameraFrame before resetting the textures themselves.  If these are not cleared,
        // the GraphicsDevice holds dangling pointers that are dereferenced during the next
        // forward render pass (bindSceneTextures slot 7 for depth, slot 18 for SSAO), which
        // causes a SIGSEGV when the CameraFrame is destroyed mid-session (e.g. SSAO toggle).
        if (const auto gd = device()) {
            // Clear sceneDepthMap only when it still points at our depth texture;
            // another CameraFrame (e.g. a second camera) must not be cleared.
            if (_sceneDepthTexture && gd->sceneDepthMap() == _sceneDepthTexture.get()) {
                gd->setSceneDepthMap(nullptr);
            }
            // Clear ssaoForwardTexture unconditionally — it is set by this CameraFrame's
            // frameUpdate() and must not outlive the SSAO texture it references.
            if (_ssaoPass && gd->ssaoForwardTexture() == _ssaoPass->ssaoTexture()) {
                gd->setSsaoForwardTexture(nullptr);
            }
        }
        reset();
    }

    void RenderPassCameraFrame::reset()
    {
        _sceneTexture.reset();
        _sceneDepthTexture.reset();
        _sceneTextureHalf.reset();
        _sceneRenderTarget.reset();
        _sceneHalfRenderTarget.reset();

        clearBeforePasses();

        _prePass.reset();
        _scenePass.reset();
        _colorGrabPass.reset();
        _scenePassTransparent.reset();
        _ssaoPass.reset();
        _taaPass.reset();
        _scenePassHalf.reset();
        _bloomPass.reset();
        _dofPass.reset();
        _composePass.reset();
        _afterPass.reset();

        _sceneTextureResolved = nullptr;
        _ownedActions.clear();
    }

    CameraFrameOptions RenderPassCameraFrame::sanitizeOptions(const CameraFrameOptions& options) const
    {
        CameraFrameOptions sanitized = options;
        if (sanitized.taaEnabled || sanitized.ssaoType != SSAOTYPE_NONE || sanitized.dofEnabled) {
            sanitized.prepassEnabled = true;
        }
        return sanitized;
    }

    bool RenderPassCameraFrame::needsReset(const CameraFrameOptions& options) const
    {
        const auto& current = _options;
        return options.ssaoType != current.ssaoType ||
            options.ssaoBlurEnabled != current.ssaoBlurEnabled ||
            options.taaEnabled != current.taaEnabled ||
            options.samples != current.samples ||
            options.stencil != current.stencil ||
            options.bloomEnabled != current.bloomEnabled ||
            options.prepassEnabled != current.prepassEnabled ||
            options.sceneColorMap != current.sceneColorMap ||
            options.dofEnabled != current.dofEnabled ||
            options.dofNearBlur != current.dofNearBlur ||
            options.dofHighQuality != current.dofHighQuality ||
            !formatsEqual(options.formats, current.formats);
    }

    void RenderPassCameraFrame::update(const CameraFrameOptions& options)
    {
        auto sanitized = sanitizeOptions(options);
        if (needsReset(sanitized) || _needsReset) {
            _needsReset = false;
            reset();
        }

        _options = sanitized;
        if (!_sceneTexture) {
            setupRenderPasses(_options);
        }
    }

    void RenderPassCameraFrame::updateSourceActions(const std::vector<RenderAction*>& sourceActions,
        LayerComposition* layerComposition, Scene* scene, Renderer* renderer,
        const std::shared_ptr<RenderTarget>& targetRenderTarget)
    {
        _sourceActions = sourceActions;
        _layerComposition = layerComposition;
        _scene = scene;
        _renderer = renderer;
        _targetRenderTarget = targetRenderTarget;

        // Clear sub-passes but preserve GPU textures and post-processing passes.
        // Scene passes that reference render actions must be recreated each frame.
        // Post-processing passes (_ssaoPass, _bloomPass, _scenePassHalf, _dofPass)
        // are persisted to avoid per-frame Metal texture allocation/deallocation
        // which causes GPU memory growth.
        clearBeforePasses();
        _ownedActions.clear();

        _prePass.reset();
        _scenePass.reset();
        _colorGrabPass.reset();
        _scenePassTransparent.reset();
        _afterPass.reset();
        _composePass.reset();
        _sceneTextureResolved = nullptr;
        // Note: _ssaoPass, _taaPass, _scenePassHalf, _bloomPass, _dofPass are NOT
        // reset here — they persist across frames to avoid per-frame GPU texture
        // allocation (each creates Metal textures + render targets in constructors).

        // Also apply any option changes from CameraComponent (e.g. TAA/SSAO toggled).
        if (_cameraComponent) {
            CameraFrameOptions options;
            const auto& taa = _cameraComponent->taa();
            const auto& dof = _cameraComponent->dof();
            const auto& ssao = _cameraComponent->ssao();
            const auto& rendering = _cameraComponent->rendering();
            options.taaEnabled = taa.enabled;
            options.dofEnabled = dof.enabled;
            options.dofNearBlur = dof.nearBlur;
            options.dofHighQuality = dof.highQuality;
            if (ssao.enabled) {
                options.ssaoType = (ssao.type == SSAOTYPE_LIGHTING) ? SSAOTYPE_LIGHTING : SSAOTYPE_COMBINE;
            } else {
                options.ssaoType = SSAOTYPE_NONE;
            }
            options.ssaoBlurEnabled = ssao.blurEnabled;
            options.bloomEnabled = rendering.bloomIntensity > 0.0f;
            options.bloomIntensity = rendering.bloomIntensity;
            options.sharpness = rendering.sharpness;
            options.vignetteEnabled = rendering.vignetteEnabled;
            options.vignetteInner = rendering.vignetteInner;
            options.vignetteOuter = rendering.vignetteOuter;
            options.vignetteCurvature = rendering.vignetteCurvature;
            options.vignetteIntensity = rendering.vignetteIntensity;
            auto sanitized = sanitizeOptions(options);

            if (needsReset(sanitized)) {
                // Options changed (e.g. TAA toggled on/off) — full reset including textures.
                reset();
                _options = sanitized;
                setupRenderPasses(_options);
                return;
            }
            _options = sanitized;
        }

        // Rebuild sub-passes using existing textures.
        createPasses(_options);

        for (const auto& pass : collectPasses()) {
            if (pass) {
                addBeforePass(pass);
            }
        }
    }

    void RenderPassCameraFrame::setRenderTargetScale(const float value)
    {
        _renderTargetScale = value;
        if (_sceneOptions) {
            _sceneOptions->scaleX = value;
            _sceneOptions->scaleY = value;
        }
    }

    std::shared_ptr<RenderTarget> RenderPassCameraFrame::createRenderTarget(const std::string& name, const bool depth,
        const bool stencil, const int samples) const
    {
        const auto gd = device();
        if (!gd) {
            return nullptr;
        }

        TextureOptions textureOptions;
        textureOptions.name = name;
        textureOptions.width = 4;
        textureOptions.height = 4;
        textureOptions.format = _hdrFormat;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        auto colorTexture = std::make_shared<Texture>(gd.get(), textureOptions);
        colorTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        colorTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = gd.get();
        rtOptions.colorBuffer = colorTexture.get();
        rtOptions.depth = depth;
        rtOptions.stencil = stencil;
        rtOptions.samples = samples;
        rtOptions.name = name;
        return gd->createRenderTarget(rtOptions);
    }

    std::shared_ptr<RenderAction> RenderPassCameraFrame::cloneActionWithTarget(const RenderAction* source,
        const std::shared_ptr<RenderTarget>& renderTarget)
    {
        if (!source) {
            return nullptr;
        }
        auto cloned = std::make_shared<RenderAction>(*source);
        cloned->renderTarget = renderTarget;
        return cloned;
    }

    int RenderPassCameraFrame::findActionIndex(const int targetLayerId, const bool targetTransparent, const int fromIndex) const
    {
        for (int i = std::max(fromIndex, 0); i < static_cast<int>(_sourceActions.size()); ++i) {
            const auto* action = _sourceActions[i];
            if (!action || !action->layer || action->layer->id() == LAYERID_DEPTH) {
                continue;
            }
            if (action->layer->id() == targetLayerId && action->transparent == targetTransparent) {
                return i;
            }
        }
        return -1;
    }

    int RenderPassCameraFrame::appendActionsToPass(const std::shared_ptr<RenderPassForward>& pass, const int fromIndex,
        const int toIndex, const std::shared_ptr<RenderTarget>& target, const bool firstLayerClears)
    {
        if (!pass || fromIndex < 0 || toIndex < fromIndex || _sourceActions.empty()) {
            return fromIndex - 1;
        }

        bool isFirst = true;
        int lastAddedIndex = fromIndex - 1;
        const int clampedTo = std::min(toIndex, static_cast<int>(_sourceActions.size()) - 1);
        for (int i = fromIndex; i <= clampedTo; ++i) {
            auto* action = _sourceActions[i];
            if (!action || !action->layer || action->layer->id() == LAYERID_DEPTH) {
                continue;
            }
            auto cloned = cloneActionWithTarget(action, target);
            if (!cloned) {
                continue;
            }

            // when firstLayerClears is false, the first action in
            // this pass should only use layer-level clear flags (not camera flags).
            // This prevents the after-pass from clearing the compose output on the
            // back buffer.
            if (isFirst && !firstLayerClears) {
                cloned->setupClears(nullptr, action->layer);
                isFirst = false;
            }

            pass->addRenderAction(cloned.get());
            _ownedActions.push_back(cloned);
            lastAddedIndex = i;
        }
        return lastAddedIndex;
    }

    void RenderPassCameraFrame::setupRenderPasses(const CameraFrameOptions& options)
    {
        if (!_cameraComponent || !_renderer || !_scene) {
            return;
        }

        const auto gd = device();
        if (!gd) {
            return;
        }

        _hdrFormat = PixelFormat::PIXELFORMAT_RGBA16F;
        _bloomEnabled = options.bloomEnabled && _hdrFormat != PixelFormat::PIXELFORMAT_RGBA8;
        _sceneHalfEnabled = _bloomEnabled || options.dofEnabled;

        // Create the scene color texture explicitly so it survives beyond the helper.
        // The createRenderTarget() helper creates a local Texture that is destroyed when
        // the method returns (RenderTarget stores only a raw pointer). We must keep
        // the color texture alive ourselves.
        // Start at the device size (not 4×4) so the first frame isn't too small.
        const auto [initW, initH] = gd->size();
        {
            TextureOptions colorOpts;
            colorOpts.name = "SceneColor";
            colorOpts.width = std::max(initW, 1);
            colorOpts.height = std::max(initH, 1);
            colorOpts.format = _hdrFormat;
            colorOpts.mipmaps = false;
            colorOpts.minFilter = FilterMode::FILTER_LINEAR;
            colorOpts.magFilter = FilterMode::FILTER_LINEAR;
            _sceneTexture = std::make_shared<Texture>(gd.get(), colorOpts);
            _sceneTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            _sceneTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        }

        TextureOptions sceneDepthOptions;
        sceneDepthOptions.name = "CameraFrameSceneDepth";
        sceneDepthOptions.width = std::max(_sceneTexture ? static_cast<int>(_sceneTexture->width()) : 1, 1);
        sceneDepthOptions.height = std::max(_sceneTexture ? static_cast<int>(_sceneTexture->height()) : 1, 1);
        sceneDepthOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
        sceneDepthOptions.mipmaps = false;
        sceneDepthOptions.minFilter = FilterMode::FILTER_NEAREST;
        sceneDepthOptions.magFilter = FilterMode::FILTER_NEAREST;
        _sceneDepthTexture = std::make_shared<Texture>(gd.get(), sceneDepthOptions);
        _sceneDepthTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _sceneDepthTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions sceneTargetOptions;
        sceneTargetOptions.graphicsDevice = gd.get();
        sceneTargetOptions.colorBuffer = _sceneTexture.get();
        sceneTargetOptions.depthBuffer = _sceneDepthTexture.get();
        sceneTargetOptions.stencil = options.stencil;
        sceneTargetOptions.samples = options.samples;
        sceneTargetOptions.name = "CameraFrameSceneTarget";
        _sceneRenderTarget = gd->createRenderTarget(sceneTargetOptions);

        if (_sceneHalfEnabled) {
            // Create half-resolution color texture explicitly (same lifetime fix as _sceneTexture).
            TextureOptions halfOpts;
            halfOpts.name = "SceneColorHalf";
            halfOpts.width = 4;
            halfOpts.height = 4;
            halfOpts.format = _hdrFormat;
            halfOpts.mipmaps = false;
            halfOpts.minFilter = FilterMode::FILTER_LINEAR;
            halfOpts.magFilter = FilterMode::FILTER_LINEAR;
            _sceneTextureHalf = std::make_shared<Texture>(gd.get(), halfOpts);
            _sceneTextureHalf->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            _sceneTextureHalf->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

            RenderTargetOptions halfTargetOptions;
            halfTargetOptions.graphicsDevice = gd.get();
            halfTargetOptions.colorBuffer = _sceneTextureHalf.get();
            halfTargetOptions.depth = false;
            halfTargetOptions.stencil = false;
            halfTargetOptions.samples = 1;
            halfTargetOptions.name = "SceneColorHalf";
            _sceneHalfRenderTarget = gd->createRenderTarget(halfTargetOptions);
        }

        _sceneOptions = std::make_shared<RenderPassOptions>();
        if (_targetRenderTarget && _targetRenderTarget->colorBuffer()) {
            _sceneOptions->resizeSource = std::shared_ptr<Texture>(_targetRenderTarget->colorBuffer(), [](Texture*) {});
        }
        _sceneOptions->scaleX = _renderTargetScale;
        _sceneOptions->scaleY = _renderTargetScale;

        createPasses(options);

        clearBeforePasses();
        for (const auto& pass : collectPasses()) {
            if (pass) {
                addBeforePass(pass);
            }
        }
    }

    std::vector<std::shared_ptr<RenderPass>> RenderPassCameraFrame::collectPasses() const
    {
        // DEVIATION: upstream orders SSAO before the scene pass because the prepass
        // writes valid depth data first. Our prepass execute() is a stub (full depth-only
        // mesh submission not yet ported), so the depth texture is empty when SSAO runs.
        // Moving SSAO after the scene passes ensures it reads valid depth from the
        // opaque+transparent scene render. This produces correct AO from final scene depth.
        return {_prePass, _scenePass, _colorGrabPass, _scenePassTransparent, _ssaoPass, _taaPass, _scenePassHalf,
            _bloomPass, _dofPass, _composePass, _afterPass};
    }

    void RenderPassCameraFrame::createPasses(const CameraFrameOptions& options)
    {
        setupScenePrepass(options);
        setupSsaoPass(options);
        const auto scenePassesInfo = setupScenePass(options);
        auto* sceneTextureWithTaa = setupTaaPass(options);
        setupSceneHalfPass(options, sceneTextureWithTaa);
        setupBloomPass(options, _sceneTextureHalf.get());
        setupDofPass(options, _sceneTexture.get(), _sceneTextureHalf.get());
        setupComposePass(options);
        setupAfterPass(options, scenePassesInfo);
    }

    void RenderPassCameraFrame::setupScenePrepass(const CameraFrameOptions& options)
    {
        if (options.prepassEnabled) {
            _prePass = std::make_shared<RenderPassPrepass>(device(), _scene, _renderer, _cameraComponent,
                _sceneDepthTexture.get(), _sceneOptions);
        }
    }

    RenderPassCameraFrame::ScenePassesInfo RenderPassCameraFrame::setupScenePass(const CameraFrameOptions& options)
    {
        ScenePassesInfo info;
        if (!_layerComposition || !_sceneRenderTarget) {
            return info;
        }

        _scenePass = std::make_shared<RenderPassForward>(device(), _layerComposition, _scene, _renderer);
        _scenePass->setHdrPass(true);
        _scenePass->init(_sceneRenderTarget, _sceneOptions);

        const int lastLayerId = options.sceneColorMap ? options.lastGrabLayerId : options.lastSceneLayerId;
        const bool lastLayerTransparent = options.sceneColorMap ? options.lastGrabLayerIsTransparent : options.lastSceneLayerIsTransparent;
        const int sceneEndIndex = findActionIndex(lastLayerId, lastLayerTransparent, 0);

        if (sceneEndIndex < 0) {
            return info;
        }

        info.lastAddedIndex = appendActionsToPass(_scenePass, 0, sceneEndIndex, _sceneRenderTarget);
        info.clearRenderTarget = false;

        if (options.sceneColorMap) {
            _colorGrabPass = std::make_shared<RenderPassColorGrab>(device());
            _colorGrabPass->setSource(_sceneRenderTarget);

            const int transparentEndIndex = findActionIndex(options.lastSceneLayerId, options.lastSceneLayerIsTransparent,
                std::max(info.lastAddedIndex + 1, 0));
            if (transparentEndIndex >= 0) {
                _scenePassTransparent = std::make_shared<RenderPassForward>(device(), _layerComposition, _scene, _renderer);
                _scenePassTransparent->setHdrPass(true);
                _scenePassTransparent->init(_sceneRenderTarget);
                info.lastAddedIndex = appendActionsToPass(_scenePassTransparent, info.lastAddedIndex + 1, transparentEndIndex,
                    _sceneRenderTarget);
            }
        }

        return info;
    }

    void RenderPassCameraFrame::setupSsaoPass(const CameraFrameOptions& options)
    {
        if (options.ssaoType != SSAOTYPE_NONE && _sceneTexture && _cameraComponent) {
            // Reuse existing SSAO pass to avoid per-frame Metal texture allocation.
            // The pass creates Texture + RenderTarget objects in its constructor,
            // so recreating it every frame causes GPU memory growth.
            if (!_ssaoPass) {
                _ssaoPass = std::make_shared<RenderPassSsao>(device(), _sceneTexture.get(), _cameraComponent, options.ssaoBlurEnabled);
            }

            // Update SSAO pass parameters from CameraComponent settings each frame
            const auto& ssao = _cameraComponent->ssao();
            _ssaoPass->radius = ssao.radius;
            _ssaoPass->intensity = ssao.intensity;
            _ssaoPass->power = ssao.power;
            _ssaoPass->sampleCount = ssao.samples;
            _ssaoPass->minAngle = ssao.minAngle;
            _ssaoPass->randomize = ssao.randomize;
            if (ssao.scale != _ssaoPass->scale()) {
                _ssaoPass->setScale(ssao.scale);
            }
        } else {
            _ssaoPass.reset();
        }
    }

    Texture* RenderPassCameraFrame::setupTaaPass(const CameraFrameOptions& options)
    {
        _sceneTextureResolved = _sceneTexture.get();
        if (options.taaEnabled && _sceneTexture) {
            _taaPass = _cameraComponent->ensureTaaPass(device(), _sceneTexture.get());
            if (_taaPass) {
                _taaPass->setSourceTexture(_sceneTexture.get());
                _taaPass->setDepthTexture(_sceneDepthTexture.get());
                _sceneTextureResolved = _taaPass->historyTexture().get();
            }
        }
        return _sceneTextureResolved;
    }

    void RenderPassCameraFrame::setupSceneHalfPass(const CameraFrameOptions& options, Texture* sourceTexture)
    {
        (void)options;
        if (_sceneHalfEnabled && _sceneHalfRenderTarget && _sceneTexture) {
            // Reuse existing downsample pass to avoid per-frame object churn.
            if (!_scenePassHalf) {
                RenderPassDownsample::Options downsampleOptions;
                downsampleOptions.boxFilter = true;
                downsampleOptions.removeInvalid = true;
                _scenePassHalf = std::make_shared<RenderPassDownsample>(device(), _sceneTexture.get(), downsampleOptions);
                auto passOptions = std::make_shared<RenderPassOptions>();
                passOptions->resizeSource = std::shared_ptr<Texture>(sourceTexture, [](Texture*) {});
                passOptions->scaleX = 0.5f;
                passOptions->scaleY = 0.5f;
                _scenePassHalf->init(_sceneHalfRenderTarget, passOptions);
                const Color clearBlack(0.0f, 0.0f, 0.0f, 1.0f);
                _scenePassHalf->setClearColor(&clearBlack);
            }
        } else {
            _scenePassHalf.reset();
        }
    }

    void RenderPassCameraFrame::setupBloomPass(const CameraFrameOptions& options, Texture* inputTexture)
    {
        (void)options;
        if (_bloomEnabled && inputTexture) {
            // Reuse existing bloom pass to avoid per-frame Metal texture allocation.
            // The pass creates Texture + RenderTarget objects in its constructor and
            // additional ones in frameUpdate(), so recreating every frame causes
            // GPU memory growth.
            if (!_bloomPass) {
                _bloomPass = std::make_shared<RenderPassBloom>(device(), inputTexture, _hdrFormat);
            }
        } else {
            _bloomPass.reset();
        }
    }

    void RenderPassCameraFrame::setupDofPass(const CameraFrameOptions& options, Texture* inputTexture,
        Texture* inputTextureHalf)
    {
        // Single-pass DOF: the compose shader reads the depth buffer directly and applies
        // a Poisson-disc blur. The multi-pass DOF pipeline (CoC → Downsample → Blur) is
        // NOT used — creating the _dofPass with its three sub-passes causes a black screen
        // because the parent RenderPassDof has no render target, which corrupts the Metal
        // render encoder state. DOF parameters are passed to the compose pass instead.
        (void)inputTexture;
        (void)inputTextureHalf;
        _dofPass.reset();
    }

    void RenderPassCameraFrame::setupComposePass(const CameraFrameOptions& options)
    {
        _composePass = std::make_shared<RenderPassCompose>(device());
        _composePass->sceneTexture = _sceneTextureResolved;
        _composePass->bloomTexture = _bloomPass ? _bloomPass->bloomTexture() : nullptr;
        _composePass->bloomIntensity = options.bloomIntensity;
        _composePass->taaEnabled = options.taaEnabled;
        _composePass->cocTexture = nullptr;   // multi-pass DOF disabled; single-pass uses depth directly
        _composePass->blurTexture = nullptr;
        _composePass->blurTextureUpscale = false;
        _composePass->dofEnabled = options.dofEnabled;
        _composePass->ssaoTexture = options.ssaoType == SSAOTYPE_COMBINE && _ssaoPass ? _ssaoPass->ssaoTexture() : nullptr;
        _composePass->sharpness = options.sharpness;
        _composePass->toneMapping = _scene ? _scene->toneMapping() : TONEMAP_LINEAR;
        _composePass->exposure = _scene ? _scene->exposure() : 1.0f;

        // Single-pass DOF: pass depth texture and DOF settings to compose
        if (options.dofEnabled && _cameraComponent) {
            const auto& dof = _cameraComponent->dof();
            _composePass->depthTexture = _sceneDepthTexture.get();
            _composePass->dofFocusDistance = dof.focusDistance;
            _composePass->dofFocusRange = dof.focusRange;
            _composePass->dofBlurRadius = dof.blurRadius;
            if (auto* camera = _cameraComponent->camera()) {
                _composePass->dofCameraNear = camera->nearClip();
                _composePass->dofCameraFar = camera->farClip();
            }
        }

        // Vignette
        _composePass->vignetteEnabled = options.vignetteEnabled;
        _composePass->vignetteInner = options.vignetteInner;
        _composePass->vignetteOuter = options.vignetteOuter;
        _composePass->vignetteCurvature = options.vignetteCurvature;
        _composePass->vignetteIntensity = options.vignetteIntensity;

        _composePass->init(_targetRenderTarget);
    }

    void RenderPassCameraFrame::setupAfterPass(const CameraFrameOptions& options, const ScenePassesInfo& scenePassesInfo)
    {
        (void)options;
        if (!_layerComposition) {
            return;
        }

        const int fromIndex = scenePassesInfo.lastAddedIndex + 1;
        if (fromIndex < 0 || fromIndex >= static_cast<int>(_sourceActions.size())) {
            return;
        }

        _afterPass = std::make_shared<RenderPassForward>(device(), _layerComposition, _scene, _renderer);
        _afterPass->init(_targetRenderTarget);

        // the after-pass renders on top of the compose output
        // so it must NOT clear the back buffer.  upstream achieves this by calling
        // addLayers(... firstLayerClears=false ...) which only uses layer-level clear
        // flags for the first action.  We replicate this by overriding camera-level
        // clears on the first cloned action.
        const int appended = appendActionsToPass(_afterPass, fromIndex, static_cast<int>(_sourceActions.size()) - 1, _targetRenderTarget,
            false /* firstLayerClears */);
        if (appended < fromIndex) {
            _afterPass.reset();
        }
    }

    void RenderPassCameraFrame::frameUpdate() const
    {
        RenderPass::frameUpdate();

        const auto gd = device();

        // When rendering to the back buffer (no explicit target), the child passes
        // need their offscreen render targets sized to the device/window dimensions.
        // RenderPass::frameUpdate() skips the resize when resizeSource is null and
        // the back buffer has no color buffer texture, so we handle it here.
        if (gd && _sceneRenderTarget && !_targetRenderTarget) {
            const auto [devW, devH] = gd->size();
            if (devW > 0 && devH > 0 &&
                (_sceneRenderTarget->width() != devW || _sceneRenderTarget->height() != devH)) {
                _sceneRenderTarget->resize(devW, devH);
            }
        }

        if (gd && _sceneDepthTexture) {
            gd->setSceneDepthMap(_sceneDepthTexture.get());
        }

        // When SSAO type is "lighting", bind the SSAO texture on the device
        // so the forward pass fragment shader can sample it (VT_FEATURE_SSAO).
        if (gd) {
            gd->setSsaoForwardTexture(
                _options.ssaoType == SSAOTYPE_LIGHTING && _ssaoPass
                    ? _ssaoPass->ssaoTexture() : nullptr);
        }

        if (_sceneTexture && _sceneDepthTexture) {
            assert(_sceneTexture->width() == _sceneDepthTexture->width() &&
                _sceneTexture->height() == _sceneDepthTexture->height() &&
                "CameraFrame scene color/depth textures must stay dimension-matched.");
        }

        if (_taaPass) {
            _taaPass->setDepthTexture(_sceneDepthTexture.get());
        }

        if (_composePass) {
            auto* resolvedTexture = _sceneTexture.get();
            if (_taaPass) {
                auto taaTexture = _taaPass->update();
                resolvedTexture = _taaPass->historyValid()
                    ? (taaTexture ? taaTexture.get() : _sceneTexture.get())
                    : _sceneTexture.get();
            }

            _sceneTextureResolved = resolvedTexture;
            _composePass->sceneTexture = resolvedTexture;
            if (_scenePassHalf) {
                _scenePassHalf->setSourceTexture(resolvedTexture);
            }
        }
    }
}
