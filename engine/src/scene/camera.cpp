// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025
//
#include "camera.h"

#include <algorithm>
#include <cstddef>

#include "graphNode.h"

namespace visutwin::canvas
{
    namespace
    {
        // Halton (2, 3) sample positions in [0, 1), one per frame, cycled.
        constexpr std::array<std::array<float, 2>, 16> haltonSequence = {{
            {0.5f, 0.333333f},
            {0.25f, 0.666667f},
            {0.75f, 0.111111f},
            {0.125f, 0.444444f},
            {0.625f, 0.777778f},
            {0.375f, 0.222222f},
            {0.875f, 0.555556f},
            {0.0625f, 0.888889f},
            {0.5625f, 0.037037f},
            {0.3125f, 0.370370f},
            {0.8125f, 0.703704f},
            {0.1875f, 0.148148f},
            {0.6875f, 0.481481f},
            {0.4375f, 0.814815f},
            {0.9375f, 0.259259f},
            {0.03125f, 0.592593f}
        }};
    }

    Camera::~Camera() = default;

    std::array<float, 2> Camera::jitterOffset(const int renderVersion, const int viewportWidth,
        const int viewportHeight) const
    {
        const float jitter = std::max(_jitter, 0.0f);
        if (jitter <= 0.0f) {
            return {0.0f, 0.0f};
        }
        const auto& offset = haltonSequence[static_cast<size_t>(renderVersion) % haltonSequence.size()];
        return {
            jitter * (offset[0] * 2.0f - 1.0f) / static_cast<float>(std::max(viewportWidth, 1)),
            jitter * (offset[1] * 2.0f - 1.0f) / static_cast<float>(std::max(viewportHeight, 1))
        };
    }

    float Camera::screenSize(const BoundingSphere& sphere) const
    {
        // Orthographic has no foreshortening, so the
        // sphere covers the same fraction wherever it sits.
        if (_projection != ProjectionType::Perspective) {
            if (_orthoHeight <= 0.0f) {
                return 0.0f;
            }
            return std::clamp(sphere.radius() / _orthoHeight, 0.0f, 1.0f);
        }
        if (!_node) {
            return 0.0f;
        }
        const Vector3 cameraPosition = _node->worldTransform().getTranslation();
        const float distance = (sphere.center() - cameraPosition).length();

        // Inside the sphere it fills the view; the asin below would also be out of
        // domain there.
        if (distance <= sphere.radius()) {
            return 1.0f;
        }

        // Both heights measured on a near plane one unit away, so the ratio is the
        // fraction of screen height.
        const float viewAngle = std::asin(sphere.radius() / distance);
        const float sphereViewHeight = std::tan(viewAngle);
        const float screenViewHeight =
            std::tan(_fov * 0.5f * (std::numbers::pi_v<float> / 180.0f));
        if (screenViewHeight <= 0.0f) {
            return 0.0f;
        }
        return std::min(sphereViewHeight / screenViewHeight, 1.0f);
    }

    void Camera::setNode(GraphNode* value)
    {
        _node = value;
        _ownedNode.reset();
    }

    void Camera::setOwnedNode(std::unique_ptr<GraphNode> value)
    {
        _ownedNode = std::move(value);
        _node = _ownedNode.get();
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
            const float offsetX = _projectionOffset.x;
            const float offsetY = _projectionOffset.y;

            if (_projection == ProjectionType::Perspective) {
                _projMat = Matrix4::perspective(fov(), aspectRatio(), nearClip(), farClip(), horizontalFov());

                // Off-center projection: the offset IS the frustum off-center term, i.e.
                // (right + left) / (right - left) and (top + bottom) / (top - bottom), which
                // Matrix4::frustum writes to these same two elements.
                _projMat.setElement(2, 0, offsetX);
                _projMat.setElement(2, 1, offsetY);
                _projMatSkybox = _projMat;
            } else {
                const auto y = _orthoHeight;
                const auto x = y * aspectRatio();
                _projMat = Matrix4::ortho(-x, x, -y, y, nearClip(), farClip());

                // Off-center projection: translate the ortho window by the offset in half-window
                // units. Negated so the window shifts the same visual direction as the
                // perspective case above (Matrix4::ortho stores -(r+l)/(r-l) here).
                _projMat.setElement(3, 0, -offsetX);
                _projMat.setElement(3, 1, -offsetY);

                _projMatSkybox = Matrix4::perspective(fov(), aspectRatio(), nearClip(), farClip());
                _projMatSkybox.setElement(2, 0, offsetX);
                _projMatSkybox.setElement(2, 1, offsetY);
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

    Vector3 Camera::screenToWorld(const float x, const float y, const float z, const float cw, const float ch)
    {
        const float rx = _rect.getX();
        const float ry = _rect.getY();
        const float rw = _rect.getZ();
        const float rh = _rect.getW();
        const float ndcX = (x - rx * cw) / (rw * cw) * 2.0f - 1.0f;
        const float ndcY = (1.0f - (y - (1.0f - ry - rh) * ch) / (rh * ch)) * 2.0f - 1.0f;

        const Matrix4 world = _node ? _node->worldTransform() : Matrix4::identity();
        const Matrix4 inverseViewProjection = (projectionMatrix() * world.inverse()).inverse();
        if (_projection == ProjectionType::Perspective) {
            // The point on the near plane gives the ray's direction from the camera.
            const Vector3 nearPoint = (inverseViewProjection * Vector4(ndcX, ndcY, -1.0f, 1.0f)).perspectiveDivide();
            const Vector3 cameraPosition = world.getTranslation();
            return cameraPosition + (nearPoint - cameraPosition).normalized() * z;
        }
        const float ndcZ = z / (_farClip - _nearClip) * 2.0f - 1.0f;
        return (inverseViewProjection * Vector4(ndcX, ndcY, ndcZ, 1.0f)).perspectiveDivide();
    }

    Vector3 Camera::worldToScreen(const Vector3& worldCoord, const float cw, const float ch)
    {
        const Matrix4 world = _node ? _node->worldTransform() : Matrix4::identity();
        const Matrix4 viewProjection = projectionMatrix() * world.inverse();
        const Vector4 clip = viewProjection * Vector4(worldCoord, 1.0f);

        // Clip space to [0, 1] with y down, then to canvas pixels through the camera rect.
        const float sx = (clip.getX() / clip.getW() + 1.0f) * 0.5f;
        const float sy = (1.0f - clip.getY() / clip.getW()) * 0.5f;
        const float rx = _rect.getX();
        const float ry = _rect.getY();
        const float rw = _rect.getZ();
        const float rh = _rect.getW();
        return Vector3(sx * rw * cw + rx * cw, sy * rh * ch + (1.0f - ry - rh) * ch, clip.getZ());
    }
}
