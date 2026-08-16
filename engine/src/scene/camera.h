// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include <array>
#include <cmath>
#include <numbers>

#include "constants.h"
#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "graphics/renderPassColorGrab.h"
#include "graphics/renderPassDepthGrab.h"
#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    enum class ProjectionType
    {
        // An orthographic camera projection where the frustum shape is essentially a cuboid
        Orthographic,
        // A perspective camera projection where the frustum shape is essentially pyramidal
        Perspective
    };

    class GraphNode;

    /**
     * @brief Perspective or orthographic camera with projection matrix, jitter (TAA), and render target binding.
     * @ingroup group_scene_renderer
     *
     * Camera manages the view-to-clip transformation and owns the list of RenderPass
     * instances that define how the camera's view is rendered (forward, shadow, post-process).
     */
    class Camera
    {
    public:
        ~Camera();

        float fov() const { return _fov; }
        // Guarded like setAspectRatio(): the outline renderer and the planar-reflection cameras
        // mirror the scene camera by re-assigning an unchanged fov every frame.
        void setFov(const float value)
        {
            if (_fov != value) {
                _fov = value;
                _projMatDirty = true;
            }
        }

        ProjectionType projection() const { return _projection; }
        void setProjection(ProjectionType value);

        float aspectRatio() const { return _aspectRatio; }
        void setAspectRatio(float value);
        AspectRatioMode aspectRatioMode() const { return _aspectRatioMode; }

        void setAspectRatioMode(AspectRatioMode value);

        float nearClip() const { return _nearClip; }
        void setNearClip(const float value) { _nearClip = value; _projMatDirty = true; }

        float farClip() const { return _farClip; }
        void setFarClip(const float value) { _farClip = value; _projMatDirty = true; }

        float orthoHeight() const { return _orthoHeight; }
        void setOrthoHeight(const float value) { _orthoHeight = value; _projMatDirty = true; }

        /**
         * The offset of the projection window from the view direction, creating an off-center
         * (asymmetric) projection — a "shift lens". Expressed in half-frustum units: an offset of
         * (0, 1) moves the projection window up by half the frustum height. Applies to both
         * perspective and orthographic projections. Defaults to (0, 0).
         *
         * A typical use is perspective correction in architectural views — keep the camera level
         * so vertical lines stay parallel, and shift the window up to frame a tall subject:
         *
         *     const float fovY = camera->fov() * DEG_TO_RAD;
         *     const float shift = std::tan(pitch * DEG_TO_RAD) / std::tan(fovY * 0.5f);
         *     camera->setProjectionOffset(Vector2(0.0f, shift));
         */
        const Vector2& projectionOffset() const { return _projectionOffset; }
        void setProjectionOffset(const Vector2& value) { _projectionOffset = value; _projMatDirty = true; }

        bool horizontalFov() const { return _horizontalFov; }

        const Matrix4& projectionMatrix()
        {
            evaluateProjectionMatrix();
            return _projMat;
        }
        const Matrix4& projectionMatrixSkybox()
        {
            evaluateProjectionMatrix();
            return _projMatSkybox;
        }

        float jitter() const { return _jitter; }
        void setJitter(const float value) { _jitter = value; }

        const Matrix4& viewProjectionPrevious() const { return _viewProjPrevious; }
        const Matrix4& viewProjectionInverse() const { return _viewProjInverse; }
        const std::array<float, 4>& jitters() const { return _jitters; }
        void storeShaderMatrices(const Matrix4& viewProjection, float jitterX, float jitterY, int renderVersion);

        const std::vector<std::shared_ptr<RenderPass>>& renderPasses() const { return _renderPasses; };

        const std::shared_ptr<RenderTarget>& renderTarget() const { return _renderTarget; }
        void setRenderTarget(const std::shared_ptr<RenderTarget>& value) { _renderTarget = value;}

        const Color& clearColor() const { return _clearColor; }
        void setClearColor(const Color& value) { _clearColor = value; }

        // Per-camera tone mapping (upstream CameraComponent::toneMapping). TONEMAP_INHERIT
        // keeps the scene-wide Scene::toneMapping value, which is what cameras did before
        // this existed; any other value overrides it for everything this camera renders.
        // The HDR camera-frame path has its own setting in CameraComponent::RenderingSettings.
        int toneMapping() const { return _toneMapping; }
        void setToneMapping(const int value) { _toneMapping = value; }

        // normalized viewport rectangle (x, y, width, height)
        // in render target coordinates.
        const Vector4& rect() const { return _rect; }
        void setRect(const Vector4& value) { _rect = value; }

        // normalized camera scissor rectangle.
        // If not explicitly set in a scene, this matches rect usage in renderer path.
        const Vector4& scissorRect() const { return _scissorRect; }
        void setScissorRect(const Vector4& value) { _scissorRect = value; }

        GraphNode* node() const { return _node; }
        // Non-owning: the node (typically an Entity) is owned elsewhere.
        void setNode(GraphNode* value);
        // Ownership variant for private cameras (shadow/light cameras) whose node
        // exists only for this camera.
        void setOwnedNode(std::unique_ptr<GraphNode> value);

        void setScissorRectClear(bool value) { _scissorRectClear = value; }

        void setClearDepthBuffer(bool value) { _clearDepthBuffer = value; }

        void setClearStencilBuffer(bool value) { _clearStencilBuffer = value; }

        void setClearColorBuffer(bool value) { _clearColorBuffer = value; }
        bool clearColorBufferEnabled() const { return _clearColorBuffer; }
        bool clearDepthBufferEnabled() const { return _clearDepthBuffer; }
        bool clearStencilBufferEnabled() const { return _clearStencilBuffer; }

        const std::shared_ptr<RenderPassColorGrab>& renderPassColorGrab() const { return _renderPassColorGrab; }

        const std::shared_ptr<RenderPass>& renderPassDepthGrab() const { return _renderPassDepthGrab; }

        void _enableRenderPassColorGrab(const std::shared_ptr<GraphicsDevice>& device, bool enable);
        void _enableRenderPassDepthGrab(const std::shared_ptr<GraphicsDevice>& device, bool enable);

        // Returns 8 frustum corners in camera-local space for the given near/far depth slice.
        // Points 0-3 are near plane corners (BR, TR, TL, BL), points 4-7 are far plane.
        // .
        std::array<Vector3, 8> getFrustumCorners(const float nearDist, const float farDist) const
        {
            const float fovRad = _fov * (std::numbers::pi_v<float> / 180.0f);
            float xNear, yNear, xFar, yFar;

            if (_projection == ProjectionType::Perspective) {
                if (_horizontalFov) {
                    xNear = nearDist * std::tan(fovRad * 0.5f);
                    yNear = xNear / _aspectRatio;
                    xFar = farDist * std::tan(fovRad * 0.5f);
                    yFar = xFar / _aspectRatio;
                } else {
                    yNear = nearDist * std::tan(fovRad * 0.5f);
                    xNear = yNear * _aspectRatio;
                    yFar = farDist * std::tan(fovRad * 0.5f);
                    xFar = yFar * _aspectRatio;
                }
            } else {
                yNear = yFar = _orthoHeight;
                xNear = xFar = _orthoHeight * _aspectRatio;
            }

            // Centre of the projection window, displaced for off-center projections. The offset is
            // in half-frustum units, so it scales with the half-extent at each depth slice.
            const float cxNear = _projectionOffset.x * xNear;
            const float cyNear = _projectionOffset.y * yNear;
            const float cxFar = _projectionOffset.x * xFar;
            const float cyFar = _projectionOffset.y * yFar;

            return {{
                Vector3(cxNear + xNear, cyNear - yNear, -nearDist),  // 0: near bottom-right
                Vector3(cxNear + xNear, cyNear + yNear, -nearDist),  // 1: near top-right
                Vector3(cxNear - xNear, cyNear + yNear, -nearDist),  // 2: near top-left
                Vector3(cxNear - xNear, cyNear - yNear, -nearDist),  // 3: near bottom-left
                Vector3(cxFar  + xFar,  cyFar  - yFar,  -farDist),   // 4: far bottom-right
                Vector3(cxFar  + xFar,  cyFar  + yFar,  -farDist),   // 5: far top-right
                Vector3(cxFar  - xFar,  cyFar  + yFar,  -farDist),   // 6: far top-left
                Vector3(cxFar  - xFar,  cyFar  - yFar,  -farDist),   // 7: far bottom-left
            }};
        }
    private:
        void evaluateProjectionMatrix();

        AspectRatioMode _aspectRatioMode = AspectRatioMode::ASPECT_AUTO;

        float _aspectRatio = 16 / 9.0f;

        float _fov = 45.0f;

        float _nearClip = 0.1f;
        float _farClip = 1000.0f;

        float _orthoHeight = 10.0f;

        ProjectionType _projection = ProjectionType::Perspective;

        // Off-center projection offset, in half-frustum units. See setProjectionOffset().
        Vector2 _projectionOffset;

        bool _horizontalFov = false;

        Matrix4 _projMat;
        bool _projMatDirty = true;

        Matrix4 _projMatSkybox;

        // camera jitter and matrices used by TAA resolve.
        float _jitter = 0.0f;
        int _shaderMatricesVersion = -1;
        Matrix4 _viewProjInverse = Matrix4::identity();
        Matrix4 _viewProjCurrent = Matrix4::identity();
        Matrix4 _viewProjPrevious = Matrix4::identity();
        std::array<float, 4> _jitters = {0.0f, 0.0f, 0.0f, 0.0f};
        bool _hasViewProjCurrent = false;

        // Render passes used to render this camera. If empty, the camera will render using the default render pass
        std::vector<std::shared_ptr<RenderPass>> _renderPasses;

        std::shared_ptr<RenderTarget> _renderTarget;

        Color _clearColor = Color(0.75f, 0.75f, 0.75f, 1.0f);
        int _toneMapping = TONEMAP_INHERIT;
        Vector4 _rect = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
        Vector4 _scissorRect = Vector4(0.0f, 0.0f, 1.0f, 1.0f);

        GraphNode* _node = nullptr;
        std::unique_ptr<GraphNode> _ownedNode;

        bool _scissorRectClear = false;

        bool _clearDepthBuffer = true;

        bool _clearStencilBuffer = true;

        bool _clearColorBuffer = true;

        std::shared_ptr<RenderPassColorGrab> _renderPassColorGrab;

        std::shared_ptr<RenderPass> _renderPassDepthGrab;

        // when true, the forward shader outputs distance-from-reflection-plane
        // instead of PBR lighting. Used by the depth camera in blurred planar reflections.
        // camera.setShaderPass('planar_reflection_depth').
        bool _planarReflectionDepthPass = false;
        bool _lightmapBakePass = false;
        bool _lightmapBakeAccumulate = false;

        // Debug shader pass selection. See setDebugShaderPass().
        DebugShaderPass _debugShaderPass = DebugShaderPass::DEBUGPASS_NONE;
    public:
        void setPlanarReflectionDepthPass(bool v) { _planarReflectionDepthPass = v; }
        [[nodiscard]] bool planarReflectionDepthPass() const { return _planarReflectionDepthPass; }

        // Lightmap bake pass (upstream's UV-space lightmapper render). Meshes drawn by
        // this camera rasterize across their own UV1 unwrap instead of through the view
        // projection, and the fragment stage outputs the diffuse light reaching each
        // texel rather than a shaded pixel. See GpuLightmapper.
        void setLightmapBakePass(bool v) { _lightmapBakePass = v; }
        [[nodiscard]] bool lightmapBakePass() const { return _lightmapBakePass; }

        // Accumulating bake pass: the draw blends additively into the lightmap instead
        // of replacing it, which is how the ambient-occlusion virtual lights sum up.
        void setLightmapBakeAccumulate(bool v) { _lightmapBakeAccumulate = v; }
        [[nodiscard]] bool lightmapBakeAccumulate() const { return _lightmapBakeAccumulate; }

        /**
         * Replaces this camera's forward output with a single surface quantity, for inspecting
         * what the material frontend produced. DEBUGPASS_NONE (the default) renders normally.
         *
         * Only affects lit materials: unlit ones return their base color before any of these
         * quantities exist. Values are written directly, so they read exactly when the camera has
         * no postprocessing — with a CameraFrame pass active the compose stage still applies
         * tonemapping and gamma on top.
         */
        void setDebugShaderPass(const DebugShaderPass value) { _debugShaderPass = value; }
        [[nodiscard]] DebugShaderPass debugShaderPass() const { return _debugShaderPass; }
    };
}
