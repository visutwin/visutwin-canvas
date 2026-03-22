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

    class Camera
    {
    public:
        float fov() const { return _fov; }
        void setFov(float value) { _fov = value; }

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

        // normalized viewport rectangle (x, y, width, height)
        // in render target coordinates.
        const Vector4& rect() const { return _rect; }
        void setRect(const Vector4& value) { _rect = value; }

        // normalized camera scissor rectangle.
        // If not explicitly set in a scene, this matches rect usage in renderer path.
        const Vector4& scissorRect() const { return _scissorRect; }
        void setScissorRect(const Vector4& value) { _scissorRect = value; }

        const std::unique_ptr<GraphNode>& node() const { return _node; }
        void setNode(GraphNode* value);

        void setScissorRectClear(bool value) { _scissorRectClear = value; }

        void setClearDepthBuffer(bool value) { clearDepthBuffer = value; }

        void setClearStencilBuffer(bool value) { clearStencilBuffer = value; }

        void setClearColorBuffer(bool value) { clearColorBuffer = value; }
        bool clearColorBufferEnabled() const { return clearColorBuffer; }
        bool clearDepthBufferEnabled() const { return clearDepthBuffer; }
        bool clearStencilBufferEnabled() const { return clearStencilBuffer; }

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

            return {{
                Vector3( xNear, -yNear, -nearDist),  // 0: near bottom-right
                Vector3( xNear,  yNear, -nearDist),  // 1: near top-right
                Vector3(-xNear,  yNear, -nearDist),  // 2: near top-left
                Vector3(-xNear, -yNear, -nearDist),  // 3: near bottom-left
                Vector3( xFar,  -yFar,  -farDist),   // 4: far bottom-right
                Vector3( xFar,   yFar,  -farDist),   // 5: far top-right
                Vector3(-xFar,   yFar,  -farDist),   // 6: far top-left
                Vector3(-xFar,  -yFar,  -farDist),   // 7: far bottom-left
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
        Vector4 _rect = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
        Vector4 _scissorRect = Vector4(0.0f, 0.0f, 1.0f, 1.0f);

        std::unique_ptr<GraphNode> _node;

        bool _scissorRectClear = false;

        bool clearDepthBuffer = true;

        bool clearStencilBuffer = true;

        bool clearColorBuffer = true;

        std::shared_ptr<RenderPassColorGrab> _renderPassColorGrab;

        std::shared_ptr<RenderPass> _renderPassDepthGrab;

        // when true, the forward shader outputs distance-from-reflection-plane
        // instead of PBR lighting. Used by the depth camera in blurred planar reflections.
        // camera.setShaderPass('planar_reflection_depth').
        bool _planarReflectionDepthPass = false;
    public:
        void setPlanarReflectionDepthPass(bool v) { _planarReflectionDepthPass = v; }
        [[nodiscard]] bool planarReflectionDepthPass() const { return _planarReflectionDepthPass; }
    };
}
