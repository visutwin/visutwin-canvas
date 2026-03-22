// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include <algorithm>
#include <vector>

#include "core/math/matrix4.h"
#include "framework/components/component.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class RenderPassCameraFrame;
    class RenderPassTAA;

    struct CameraComponentData
    {
    };

    struct DofSettings
    {
        bool enabled = false;
        bool nearBlur = false;
        float focusDistance = 100.0f;
        float focusRange = 10.0f;
        float blurRadius = 3.0f;
        int blurRings = 4;
        int blurRingPoints = 5;
        bool highQuality = true;
    };

    struct TaaSettings
    {
        bool enabled = false;
        bool highQuality = true;
        float jitter = 1.0f;
    };

    // SSAO settings matching upstream CameraFrame.ssao
    struct SsaoSettings
    {
        bool enabled = false;
        bool blurEnabled = true;
        float intensity = 0.5f;
        float power = 6.0f;
        float radius = 30.0f;
        int samples = 12;
        float minAngle = 10.0f;
        float scale = 1.0f;
        bool randomize = false;
        // SSAO compositing mode:
        //   "combine"  — post-process compose pass (default, multiplies final image)
        //   "lighting" — per-material forward pass (modulates AO during PBR lighting)
        std::string_view type = "combine";
    };

    // Rendering settings matching upstream CameraFrame.rendering
    struct RenderingSettings
    {
        float renderTargetScale = 1.0f;
        float sharpness = 0.0f;
        float bloomIntensity = 0.0f;  // 0 = disabled
        int toneMapping = TONEMAP_LINEAR;
    };

    /*
    * The CameraComponent enables an Entity to render the scene. A scene requires at least
    * one enabled camera component to be rendered. The camera's view direction is along the negative
    * z-axis of the owner entity.
    */
    class CameraComponent : public Component
    {
    public:
        CameraComponent(IComponentSystem* system, Entity* entity);
        ~CameraComponent();

        static const std::vector<CameraComponent*>& instances() { return _instances; }

        const Matrix4& projectionMatrix() const { return _camera->projectionMatrix(); }

        void initializeComponentData() override;

        // Gets the render passes the camera uses for rendering, instead of its default rendering
        const std::vector<std::shared_ptr<RenderPass>>& renderPasses() const { return _camera->renderPasses(); }

        bool renderSceneColorMap() const { return _renderSceneColorMap > 0; }

        bool renderSceneDepthMap() const { return _renderSceneDepthMap > 0; }

        void requestSceneColorMap(bool enabled);
        void requestSceneDepthMap(bool enabled);

        Camera* camera() { return _camera; }

        const Camera* camera() const { return _camera; }

        void* onPostprocessing() const
        {
            return (_dof.enabled || _taa.enabled || _ssao.enabled || _rendering.bloomIntensity > 0.0f)
                ? const_cast<CameraComponent*>(this) : nullptr;
        }

        const std::vector<int>& layers() const { return _layers; }
        void setLayers(const std::vector<int>& layers) { _layers = layers; }

        const DofSettings& dof() const { return _dof; }
        DofSettings& dof() { return _dof; }
        void setDofEnabled(bool enabled);
        void setDof(const DofSettings& value)
        {
            setDofEnabled(value.enabled);
            _dof = value;
        }
        std::shared_ptr<RenderTarget> dofSceneRenderTarget() const { return _dofSceneRenderTarget; }

        const TaaSettings& taa() const { return _taa; }
        TaaSettings& taa() { return _taa; }
        void setTaaEnabled(bool enabled);
        void setTaa(const TaaSettings& value)
        {
            setTaaEnabled(value.enabled);
            _taa = value;
            if (_camera) {
                _camera->setJitter(_taa.enabled ? std::max(_taa.jitter, 0.0f) : 0.0f);
            }
        }
        std::shared_ptr<RenderPassTAA> ensureTaaPass(const std::shared_ptr<GraphicsDevice>& device, Texture* sourceTexture);

        const SsaoSettings& ssao() const { return _ssao; }
        SsaoSettings& ssao() { return _ssao; }
        void setSsaoEnabled(bool enabled);
        void setSsao(const SsaoSettings& value)
        {
            setSsaoEnabled(value.enabled);
            _ssao = value;
        }

        const RenderingSettings& rendering() const { return _rendering; }
        RenderingSettings& rendering() { return _rendering; }
        void setRendering(const RenderingSettings& value) { _rendering = value; }

        // Persistent camera frame for postprocessing (TAA, DOF, etc.)
        std::shared_ptr<RenderPassCameraFrame> cameraFrame() const { return _cameraFrame; }
        void setCameraFrame(const std::shared_ptr<RenderPassCameraFrame>& frame) { _cameraFrame = frame; }

        bool rendersLayer(const int layerId) const
        {
            if (_layers.empty()) {
                return true;
            }
            return std::find(_layers.begin(), _layers.end(), layerId) != _layers.end();
        }

    private:
        static std::vector<CameraComponent*> _instances;

        Camera* _camera = nullptr;

        // A counter of requests of color map rendering
        int _renderSceneColorMap = 0;

        // A counter of requests of depth map rendering
        int _renderSceneDepthMap = 0;

        DofSettings _dof;
        TaaSettings _taa;
        SsaoSettings _ssao;
        RenderingSettings _rendering;
        std::shared_ptr<Texture> _dofSceneColorTexture;
        std::shared_ptr<RenderTarget> _dofSceneRenderTarget;
        std::shared_ptr<RenderPassTAA> _taaPass;
        std::shared_ptr<RenderPassCameraFrame> _cameraFrame;
        void ensureDofRenderTarget();
        bool requiresPostprocessRenderTarget() const { return _dof.enabled || _taa.enabled || _ssao.enabled; }
        void updatePostprocessRenderTargetBinding() const;

        // default camera layers include world, depth, skybox, UI and immediate.
        std::vector<int> _layers = {LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX, LAYERID_UI, LAYERID_IMMEDIATE};
    };
}
