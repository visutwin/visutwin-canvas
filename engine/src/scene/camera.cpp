// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#include "camera.h"

#include "graphNode.h"

namespace visutwin::canvas
{
    void Camera::setNode(GraphNode* value)
    {
        _node = std::unique_ptr<GraphNode>(value);
    }

    void Camera::setProjection(ProjectionType value)
    {
        if (_projection != value) {
            _projection = value;
            _projMatDirty = true;
        }
    }

    void Camera::setAspectRatio(float value)
    {
        if (_aspectRatio != value) {
            _aspectRatio = value;
            _projMatDirty = true;
        }
    }

    void Camera::setAspectRatioMode(AspectRatioMode value)
    {
        if (_aspectRatioMode != value) {
            _aspectRatioMode = value;
            _projMatDirty = true;
        }
    }

    void Camera::evaluateProjectionMatrix()
    {
        if (_projMatDirty) {
            if (_projection == ProjectionType::Perspective) {
                _projMat = Matrix4::perspective(fov(), aspectRatio(), nearClip(), farClip(), horizontalFov());
                _projMatSkybox = _projMat;
            } else {
                const auto y = _orthoHeight;
                const auto x = y * aspectRatio();
                _projMat = Matrix4::ortho(-x, x, -y, y, nearClip(), farClip());
                _projMatSkybox = Matrix4::perspective(fov(), aspectRatio(), nearClip(), farClip());
            }

            _projMatDirty = false;
        }
    }

    void Camera::storeShaderMatrices(const Matrix4& viewProjection, const float jitterX, const float jitterY,
        const int renderVersion)
    {
        if (_shaderMatricesVersion == renderVersion) {
            return;
        }

        _shaderMatricesVersion = renderVersion;
        _viewProjPrevious = _hasViewProjCurrent ? _viewProjCurrent : viewProjection;
        _viewProjCurrent = viewProjection;
        _hasViewProjCurrent = true;
        _viewProjInverse = viewProjection.inverse();

        _jitters[2] = _jitters[0];
        _jitters[3] = _jitters[1];
        _jitters[0] = jitterX;
        _jitters[1] = jitterY;
    }

    void Camera::_enableRenderPassColorGrab(const std::shared_ptr<GraphicsDevice>& device, const bool enable)
    {
        if (enable) {
            if (!_renderPassColorGrab) {
                _renderPassColorGrab = std::make_shared<RenderPassColorGrab>(device);
            }
        } else {
            _renderPassColorGrab.reset();
        }
    }

    void Camera::_enableRenderPassDepthGrab(const std::shared_ptr<GraphicsDevice>& device, const bool enable)
    {
        if (enable) {
            if (!_renderPassDepthGrab) {
                _renderPassDepthGrab = std::make_shared<RenderPassDepthGrab>(device, this);
            }
        } else {
            _renderPassDepthGrab.reset();
        }
    }
}
