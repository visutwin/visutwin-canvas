// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.10.2026
//
// Renders the scene from a camera mirrored by a plane (reflection) or matching the scene
// camera (refraction) into a texture, for a material to sample at its fragments' screen
// positions: planar reflections and refractions of water, mirrors and glass. The script's
// entity carries the camera, whose layers say what goes into the texture (never the layers of
// the surface that samples it); the script owns that camera's render target, transform and
// projection. Call frameUpdate() each frame once the scene camera has its final transform.
//
// With oblique clipping the camera's near plane is the plane itself (Lengyel's oblique
// frustum), so nothing on the wrong side of the surface reaches the texture: below it for a
// reflection, above it for a refraction.
//
// The texture is RGBA8 holding the camera's gamma-encoded output, so a reader decodes it, or
// with hdr RGBA16F holding linear colours (the camera's own exposure, tone mapping and gamma
// set aside: Camera::setLinearOutput). A StandardMaterial reflects it through
// setPlanarReflectionMap and setPlanarReflectionMatrix(viewProjectionMatrix()); with mipmaps a
// rough surface reads a blurred mip. The camera takes the scene camera's jitter, so that
// camera's temporal antialiasing antialiases the reflection too (the material samples the
// texture with the projection without the jitter).
// DEVIATIONS: upstream renders into an sRGB texture the sampler decodes, and its entity fires
// "onPlanarPostRender", which no user here needs yet.
//
#pragma once

#include <array>
#include <memory>

#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "framework/destroyWatch.h"
#include "framework/script/script.h"
#include "framework/script/scriptRegistry.h"
#include "platform/graphics/constants.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Entity;
    class RenderTarget;
    class Texture;

    class PlanarRenderer : public Script
    {
    public:
        SCRIPT_NAME("PlanarRenderer")

        enum class Mode
        {
            Reflection,
            Refraction
        };

        PlanarRenderer();
        ~PlanarRenderer() override;

        void initialize() override;

        /// The entity of the scene's main camera, which this camera follows.
        void setSceneCameraEntity(Entity* entity);
        [[nodiscard]] Entity* sceneCameraEntity() const { return _sceneCameraEntity; }

        void setMode(const Mode mode) { _mode = mode; }
        [[nodiscard]] Mode mode() const { return _mode; }

        /// The texture's size relative to the scene camera's target (0.05-1, default 0.5).
        void setScale(const float value) { _scale = value; }
        [[nodiscard]] float scale() const { return _scale; }

        /// A depth buffer for the render (default on).
        void setDepth(const bool value) { _depth = value; }

        /// A mipmapped texture, the mips generated after each render (default off): what a
        /// rough surface reading the planar reflection blurs with.
        void setMipmaps(const bool value) { _mipmaps = value; }
        [[nodiscard]] bool mipmaps() const { return _mipmaps; }

        /// The filter generating those mips (default the Gaussian one): the blurred reflections
        /// of rough surfaces sample the small levels, which the Gaussian filter keeps smooth
        /// where the box filter shows blocks that flicker as the camera moves, at a higher
        /// cost of generating them (Texture::mipmapFilter). Applies to the next render.
        void setMipmapFilter(const MipmapFilter value) { _mipmapFilter = value; }
        [[nodiscard]] MipmapFilter mipmapFilter() const { return _mipmapFilter; }

        /// A high dynamic range texture (RGBA16F) holding linear colours, so bright lights keep
        /// their brightness in it; the camera rendering the material that reads it applies the
        /// exposure, tone mapping and gamma. The camera's own linear-output setting is restored
        /// for a low dynamic range texture. Falls back to RGBA8 on a device that cannot render
        /// half-float (default off).
        void setHdr(const bool value) { _hdr = value; }
        [[nodiscard]] bool hdr() const { return _hdr; }

        /// A point on the plane and its normal.
        void setPlane(const Vector3& point, const Vector3& normal)
        {
            _planePoint = point;
            _planeNormal = normal;
        }
        [[nodiscard]] const Vector3& planePoint() const { return _planePoint; }
        [[nodiscard]] const Vector3& planeNormal() const { return _planeNormal; }

        /// Clips the rendered scene by the plane through the projection (default on).
        void setObliqueClipping(const bool value) { _obliqueClipping = value; }
        [[nodiscard]] bool obliqueClipping() const { return _obliqueClipping; }

        /// How far the clipping plane is moved against the kept side, letting geometry just past
        /// the plane render: it hides the seam along the waterline, waves above all (default 0.05).
        void setClipBias(const float value) { _clipBias = value; }
        [[nodiscard]] float clipBias() const { return _clipBias; }

        /// Updates the render target, the camera's transform and projection, and returns the
        /// texture the camera renders to; null while the camera is disabled or missing.
        Texture* frameUpdate();

        [[nodiscard]] Texture* texture() const { return _texture.get(); }

        /// The camera's view-projection, which projects a world position into the texture:
        /// updated by frameUpdate(), for StandardMaterial::setPlanarReflectionMatrix.
        [[nodiscard]] const Matrix4& viewProjectionMatrix() const { return _viewProjectionMatrix; }

    private:
        CameraComponent* planarCamera() const;
        void updateRenderTarget();
        void updateViewSpaceClipPlane();
        void calculateProjection(Matrix4& projection) const;
        void destroyRenderTarget();
        void updateColorSettings(bool hdr);

        Entity* _sceneCameraEntity = nullptr;
        DestroyWatch _sceneCameraWatch;
        Mode _mode = Mode::Reflection;
        float _scale = 0.5f;
        bool _depth = true;
        bool _mipmaps = false;
        MipmapFilter _mipmapFilter = MipmapFilter::MIPMAPFILTER_GAUSSIAN;
        bool _hdr = false;
        // The camera's linear-output setting while it renders HDR, to restore.
        bool _ldrStored = false;
        bool _ldrLinearOutput = false;
        Matrix4 _viewProjectionMatrix = Matrix4::identity();
        Vector3 _planePoint = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 _planeNormal = Vector3(0.0f, 1.0f, 0.0f);
        bool _obliqueClipping = true;
        float _clipBias = 0.05f;
        bool _projectionInstalled = false;
        bool _warnedNoSceneCamera = false;

        std::shared_ptr<Texture> _texture;
        std::shared_ptr<RenderTarget> _renderTarget;
        // The clipping plane in the camera's view space (nx, ny, nz, d).
        std::array<float, 4> _viewSpaceClipPlane = {0.0f, 0.0f, 0.0f, 0.0f};
    };
}

REGISTER_SCRIPT(visutwin::canvas::PlanarRenderer, "PlanarRenderer")
