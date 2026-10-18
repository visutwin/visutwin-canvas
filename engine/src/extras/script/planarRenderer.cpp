// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
#include "extras/script/planarRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>

#include <spdlog/spdlog.h>

#include "core/math/matrix4.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    namespace
    {
        float signOf(const float value)
        {
            return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
        }
    }

    PlanarRenderer::PlanarRenderer() = default;

    PlanarRenderer::~PlanarRenderer()
    {
        if (CameraComponent* camera = planarCamera(); camera && camera->camera()) {
            camera->camera()->setCalculateProjection(nullptr);
            if (camera->camera()->renderTarget() == _renderTarget) {
                camera->camera()->setRenderTarget(nullptr);
            }
        }
        destroyRenderTarget();
    }

    CameraComponent* PlanarRenderer::planarCamera() const
    {
        return entity() ? entity()->findComponent<CameraComponent>() : nullptr;
    }

    void PlanarRenderer::setSceneCameraEntity(Entity* entity)
    {
        _sceneCameraEntity = entity;
        _sceneCameraWatch.watch(entity, [this]() { _sceneCameraEntity = nullptr; });
    }

    void PlanarRenderer::initialize()
    {
        if (!_sceneCameraEntity || !_sceneCameraEntity->findComponent<CameraComponent>()) {
            spdlog::error("PlanarRenderer: the scene camera entity is not set, or has no camera");
        }
        if (!planarCamera()) {
            spdlog::error("PlanarRenderer: its entity has no camera component");
        }
    }

    void PlanarRenderer::destroyRenderTarget()
    {
        _renderTarget.reset();
        _texture.reset();
    }

    void PlanarRenderer::updateRenderTarget()
    {
        CameraComponent* camera = planarCamera();
        CameraComponent* sceneCamera = _sceneCameraEntity ? _sceneCameraEntity->findComponent<CameraComponent>() : nullptr;
        Engine* engine = entity() ? entity()->engine() : nullptr;
        const auto device = engine ? engine->graphicsDevice() : nullptr;
        if (!camera || !sceneCamera || !device) {
            return;
        }

        // The scene camera's resolution, scaled and limited to the largest texture.
        int sceneWidth = 0;
        int sceneHeight = 0;
        if (const auto& target = sceneCamera->camera()->renderTarget()) {
            sceneWidth = target->width();
            sceneHeight = target->height();
        } else {
            std::tie(sceneWidth, sceneHeight) = device->size();
        }
        const int maxSize = device->maxTextureSize();
        const int width = std::clamp(static_cast<int>(std::floor(static_cast<float>(sceneWidth) * _scale)), 1, maxSize);
        const int height = std::clamp(static_cast<int>(std::floor(static_cast<float>(sceneHeight) * _scale)), 1, maxSize);

        if (_renderTarget && _texture && _texture->width() == static_cast<uint32_t>(width) &&
            _texture->height() == static_cast<uint32_t>(height) && camera->camera()->renderTarget() == _renderTarget) {
            return;
        }

        TextureOptions textureOptions;
        textureOptions.name = (entity()->name().empty() ? std::string("Planar") : entity()->name()) + ":PlanarRenderer";
        textureOptions.width = width;
        textureOptions.height = height;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(device.get(), textureOptions);
        texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device.get();
        targetOptions.colorBuffer = texture.get();
        targetOptions.depth = _depth;
        targetOptions.name = textureOptions.name + "Target";
        auto target = device->createRenderTarget(targetOptions);

        // The camera lets go of the old target before the texture behind it goes.
        camera->camera()->setRenderTarget(target);
        _renderTarget = std::move(target);
        _texture = std::move(texture);
    }

    void PlanarRenderer::calculateProjection(Matrix4& projection) const
    {
        const CameraComponent* camera = planarCamera();
        if (!camera) {
            return;
        }
        const Camera* cam = camera->camera();
        const float aspect = _texture && _texture->height() > 0
            ? static_cast<float>(_texture->width()) / static_cast<float>(_texture->height()) : 1.0f;

        // Rebuilt from the settings each time: the matrix handed in may already carry the
        // previous modification.
        projection = Matrix4::perspective(cam->fov(), aspect, cam->nearClip(), cam->farClip(), cam->horizontalFov());
        if (!_obliqueClipping) {
            return;
        }

        // The near plane replaced by the clipping plane: "Oblique View Frustum Depth Projection
        // and Clipping", Eric Lengyel. The clip-space corner opposite the plane, in camera space:
        const float cx = _viewSpaceClipPlane[0];
        const float cy = _viewSpaceClipPlane[1];
        const float cz = _viewSpaceClipPlane[2];
        const float cw = _viewSpaceClipPlane[3];
        const float qx = (signOf(cx) + projection.getElement(2, 0)) / projection.getElement(0, 0);
        const float qy = (signOf(cy) + projection.getElement(2, 1)) / projection.getElement(1, 1);
        const float qz = -1.0f;
        const float qw = (1.0f + projection.getElement(2, 2)) / projection.getElement(3, 2);

        // Scaled so the far plane of the result touches that corner, which loses the least
        // depth precision; the plane becomes the third row.
        const float s = 2.0f / (cx * qx + cy * qy + cz * qz + cw * qw);
        projection.setElement(0, 2, cx * s);
        projection.setElement(1, 2, cy * s);
        projection.setElement(2, 2, cz * s + 1.0f);
        projection.setElement(3, 2, cw * s);
    }

    void PlanarRenderer::updateViewSpaceClipPlane()
    {
        // The plane faces the kept side: above it for a reflection, below it for a refraction,
        // and moves against that side by the bias.
        Vector3 normal = _planeNormal.normalized();
        if (_mode != Mode::Reflection) {
            normal = normal * -1.0f;
        }
        const Vector3 point = _planePoint + normal * -_clipBias;
        const float dist = -normal.dot(point);

        // Into the camera's view space: transpose(W) * plane, W the camera's world transform.
        const Matrix4& w = entity()->worldTransform();
        auto& vp = _viewSpaceClipPlane;
        for (int c = 0; c < 4; ++c) {
            const Vector4 column = w.getColumn(c);
            vp[c] = column.getX() * normal.getX() + column.getY() * normal.getY() + column.getZ() * normal.getZ();
        }
        vp[3] += dist;

        // The technique needs the camera on the plane's negative side.
        if (vp[3] > 0.0f) {
            for (float& v : vp) {
                v = -v;
            }
        }
    }

    Texture* PlanarRenderer::frameUpdate()
    {
        CameraComponent* camera = planarCamera();
        CameraComponent* sceneCamera = _sceneCameraEntity ? _sceneCameraEntity->findComponent<CameraComponent>() : nullptr;
        if (!camera || !sceneCamera || !camera->camera() || !sceneCamera->camera()) {
            return nullptr;
        }

        updateRenderTarget();
        if (!camera->active()) {
            return nullptr;
        }

        const Matrix4& sceneTransform = _sceneCameraEntity->worldTransform();
        const Vector3 position = Vector3(sceneTransform.getColumn(3));
        if (_mode == Mode::Reflection) {
            // The scene camera mirrored by the plane.
            const Vector3 normal = _planeNormal.normalized();
            const Matrix4 reflection = Matrix4::reflection(normal.getX(), normal.getY(), normal.getZ(),
                -normal.dot(_planePoint));
            const Vector3 forward = Vector3(sceneTransform.getColumn(2)).normalized() * -1.0f;
            entity()->setPosition(reflection.transformPoint(position));
            entity()->lookAt(reflection.transformPoint(position + forward));
        } else {
            // The refraction camera is the scene camera.
            entity()->setPosition(position);
            entity()->setRotation(_sceneCameraEntity->rotation());
        }

        Camera* planar = camera->camera();
        const Camera* scene = sceneCamera->camera();
        planar->setFov(scene->fov());
        planar->setHorizontalFov(scene->horizontalFov());
        planar->setOrthoHeight(scene->orthoHeight());
        planar->setNearClip(scene->nearClip());
        planar->setFarClip(scene->farClip());
        planar->setAperture(scene->aperture());
        planar->setSensitivity(scene->sensitivity());
        planar->setShutter(scene->shutter());

        // Oblique clipping through the projection hook.
        if (_obliqueClipping) {
            updateViewSpaceClipPlane();
            if (!_projectionInstalled) {
                planar->setCalculateProjection([this](Matrix4& projection) { calculateProjection(projection); });
                _projectionInstalled = true;
            }
        } else if (_projectionInstalled) {
            planar->setCalculateProjection(nullptr);
            _projectionInstalled = false;
        }

        return _texture.get();
    }
}
