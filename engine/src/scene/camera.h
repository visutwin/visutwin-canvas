// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025
//
#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <numbers>
#include <optional>

#include "clipPlanes.h"
#include "constants.h"
#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "core/shape/boundingSphere.h"
#include "graphics/renderPassColorGrab.h"
#include "graphics/renderPassDepthGrab.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/sceneMapHandles.h"

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

        /// Physical camera: aperture in f-stops (default 16), shutter in seconds
        /// (1/1000) and sensitivity in ISO (1000). Read only under Scene::physicalUnits,
        /// where they replace the scene exposure (see physicalExposure()).
        float aperture() const { return _aperture; }
        void setAperture(const float value) { _aperture = value; }
        float shutter() const { return _shutter; }
        void setShutter(const float value) { _shutter = value; }
        float sensitivity() const { return _sensitivity; }
        void setSensitivity(const float value) { _sensitivity = value; }

        /// 1 / (1.2 * 2^EV100), EV100 = log2(N^2 / t * 100 / S).
        float physicalExposure() const
        {
            const float ev100 = std::log2((_aperture * _aperture) / _shutter * 100.0f / _sensitivity);
            return 1.0f / (std::pow(2.0f, ev100) * 1.2f);
        }

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

        /**
         * A function that adjusts the projection matrix whenever it is asked for: it receives
         * the matrix the camera built from its settings and may change it in place, as a planar
         * reflection's oblique near plane does. While one is set the matrix is rebuilt on every
         * request, because the function's own inputs can change without the camera knowing. The
         * skybox projection is left as built. Null (the default) removes it.
         */
        using CalculateProjection = std::function<void(Matrix4& projection)>;
        void setCalculateProjection(CalculateProjection value)
        {
            _calculateProjection = std::move(value);
            _projMatDirty = true;
        }
        const CalculateProjection& calculateProjection() const { return _calculateProjection; }

        /// The fov is the WIDTH's angle rather than the
        /// height's, which keeps a scene's width in view on a portrait window.
        bool horizontalFov() const { return _horizontalFov; }
        void setHorizontalFov(const bool value)
        {
            if (_horizontalFov != value) {
                _horizontalFov = value;
                _projMatDirty = true;
            }
        }

        /**
         * The world point under canvas point (x, y) — y down, in a
         * canvas `cw` x `ch` — through the camera's rect. Perspective: `z` is the DISTANCE from
         * the camera along the ray through the point. Orthographic: `z` picks the depth
         * between the clip planes (z / (far - near) of the way through NDC). Taken from the
         * inverse view-projection, so a projection offset is included.
         */
        Vector3 screenToWorld(float x, float y, float z, float cw, float ch);

        /**
         * The canvas point (x right, y DOWN, in a `cw` x `ch`
         * canvas) a world point projects to, through the camera's rect. z is the clip-space
         * z before the divide. A point behind the
         * camera still returns a position (mirrored through the centre); test the view-space
         * depth to reject it.
         */
        Vector3 worldToScreen(const Vector3& worldCoord, float cw, float ch);

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

        /**
         * The TAA sub-pixel offset of this camera's projection in the frame of
         * `renderVersion`, for a viewport of the given pixel size: {x, y}, to be ADDED to
         * projection elements (2, 0) and (2, 1). Zero when jitter is off. Every pass that
         * rasterises the camera's view (the forward pass, the depth prepass) takes it
         * from here, so all of them sample the same sub-pixel positions in one frame.
         */
        std::array<float, 2> jitterOffset(int renderVersion, int viewportWidth, int viewportHeight) const;

        const Matrix4& viewProjectionPrevious() const { return _viewProjPrevious; }
        const Matrix4& viewProjectionInverse() const { return _viewProjInverse; }
        const std::array<float, 4>& jitters() const { return _jitters; }
        void storeShaderMatrices(const Matrix4& viewProjection, float jitterX, float jitterY, int renderVersion);

        const std::vector<std::shared_ptr<RenderPass>>& renderPasses() const { return _renderPasses; };
        /// Passes that replace the camera's own rendering: the frame graph schedules them in
        /// order where the camera would render, after its directional shadows. A forward pass
        /// among them takes this camera's component.
        void setRenderPasses(const std::vector<std::shared_ptr<RenderPass>>& passes) { _renderPasses = passes; }

        /// Passes that run after everything this camera renders in the frame — its render
        /// passes when it uses those, its camera frame, its grabs — and before the cameras that
        /// render after it. Add or remove entries to change them; a disabled pass is skipped.
        std::vector<std::shared_ptr<RenderPass>>& afterPasses() { return _afterPasses; }
        const std::vector<std::shared_ptr<RenderPass>>& afterPasses() const { return _afterPasses; }

        const std::shared_ptr<RenderTarget>& renderTarget() const { return _renderTarget; }
        void setRenderTarget(const std::shared_ptr<RenderTarget>& value) { _renderTarget = value;}

        const Color& clearColor() const { return _clearColor; }
        void setClearColor(const Color& value) { _clearColor = value; }

        /// The clear colour of colour attachment `attachment` (1-3) of a target with several:
        /// its own when set, otherwise clearColor().
        const Color& clearColor(const int attachment) const
        {
            return attachment >= 1 && attachment <= 3 && _attachmentClearColors[attachment - 1]
                ? *_attachmentClearColors[attachment - 1] : _clearColor;
        }
        void setClearColor(const int attachment, const Color& value)
        {
            if (attachment == 0) {
                _clearColor = value;
            } else if (attachment >= 1 && attachment <= 3) {
                _attachmentClearColors[attachment - 1] = value;
            }
        }

        // Per-camera tone mapping. TONEMAP_INHERIT
        // keeps the scene-wide Scene::toneMapping value; any other value overrides it for
        // everything this camera renders.
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

        /**
         * The fraction of the viewport's HEIGHT this world-space sphere covers, in
         * [0,1]. 1 when the camera is inside the
         * sphere. Used to rank lights when more of them are visible than the shader
         * has slots: apparent size is what decides how much of the picture a light
         * is responsible for, where distance or authoring order decide nothing.
         */
        float screenSize(const BoundingSphere& sphere) const;

        /**
         * Bitmask ANDed with MeshInstance::mask() during culling.
         * All bits by default, so every instance passes. A camera
         * that wants a subset of the scene says so here instead of being given its
         * own layer.
         */
        uint32_t cullingMask() const { return _cullingMask; }
        void setCullingMask(const uint32_t value) { _cullingMask = value; }

        /// Whether the camera draws the scene's sky. The renderer sets it every frame, before
        /// culling: cleared while an active effect of the camera's frame renders the sky in its
        /// place (CameraFrameEffect::rendersSky).
        bool skyVisible() const { return _skyVisible; }
        void setSkyVisible(const bool value) { _skyVisible = value; }

        /// What the camera's exposure (the scene's, or the physical camera's under physical
        /// units) is multiplied by: set every frame by its camera frame, the product of what its
        /// active effects ask for (CameraFrameEffect::cameraExposureScale, the auto exposure),
        /// and 1 again once it renders through no frame. Scene::exposureFor applies it.
        float exposureScale() const { return _exposureScale; }
        void setExposureScale(const float value) { _exposureScale = value; }

        /**
         * Planes that clip what the camera renders, in world space (copied, at most
         * ClipPlanes::kMaxPlanes): in every pass of the camera, the depth prepass and picking
         * included, a fragment behind any of them is discarded, or under clipIntersection only
         * one behind all of them. Shadows render from their light and are not clipped, nor are
         * screen-space elements. A mesh instance with planes of its own is clipped by both
         * sets. Only the first plane compiles new shader variants.
         */
        void setClippingPlanes(std::span<const Plane> planes) { _clip.setPlanes(planes); }
        const std::vector<Plane>& clippingPlanes() const { return _clip.planes(); }
        void setClipIntersection(const bool value) { _clip.setIntersection(value); }
        bool clipIntersection() const { return _clip.intersection(); }
        const ClipPlanes& clipping() const { return _clip; }

        GraphNode* node() const { return _node; }
        // Non-owning: the node (typically an Entity) is owned elsewhere.
        void setNode(GraphNode* value);
        // Ownership variant for private cameras (shadow/light cameras) whose node
        // exists only for this camera.
        void setOwnedNode(std::unique_ptr<GraphNode> value);

        /// Copy every SETTING of `other`: projection, near and far planes, clipping
        /// planes, clears, rects, render target, tone mapping, culling mask, jitter
        /// debug pass and the physical exposure settings. Not copied: the node, the grab passes and render passes
        /// (per-camera objects its component rebuilds) and the per-frame matrices.
        void copy(const Camera& other)
        {
            _aspectRatio = other._aspectRatio;
            _aspectRatioMode = other._aspectRatioMode;
            _farClip = other._farClip;
            _nearClip = other._nearClip;
            _fov = other._fov;
            _horizontalFov = other._horizontalFov;
            _orthoHeight = other._orthoHeight;
            _projection = other._projection;
            _projectionOffset = other._projectionOffset;
            _calculateProjection = other._calculateProjection;
            _cullingMask = other._cullingMask;
            _exposureScale = other._exposureScale;
            _clearColor = other._clearColor;
            _attachmentClearColors = other._attachmentClearColors;
            _clearColorBuffer = other._clearColorBuffer;
            _clearDepthBuffer = other._clearDepthBuffer;
            _clearStencilBuffer = other._clearStencilBuffer;
            _scissorRectClear = other._scissorRectClear;
            _rect = other._rect;
            _scissorRect = other._scissorRect;
            _renderTarget = other._renderTarget;
            _toneMapping = other._toneMapping;
            _jitter = other._jitter;
            _debugShaderPass = other._debugShaderPass;
            _aperture = other._aperture;
            _shutter = other._shutter;
            _sensitivity = other._sensitivity;
            if (!(_clip == other._clip)) {
                _clip = other._clip;
            }
            _projMatDirty = true;
        }

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

        /// This camera's scene depth and colour maps: the same objects for the camera's life,
        /// naming the maps it rendered most recently (see sceneMapHandles.h). Shared, so a
        /// consumer (a Compute) may keep one past the camera; the destructor marks them.
        const std::shared_ptr<SceneDepthMapHandle>& sceneDepthMapHandle() const { return _sceneDepthMapHandle; }
        const std::shared_ptr<SceneColorMapHandle>& sceneColorMapHandle() const { return _sceneColorMapHandle; }

        /// Records the depth a producer rendered for this camera, with how it stores it, and the
        /// camera parameters, inverse view-projection and viewport it was rendered with. Only the
        /// handle: the device's global grab maps are the producer's to set.
        void publishSceneDepthMap(const std::shared_ptr<Texture>& texture, int renderVersion,
            bool linear, bool packed, bool reciprocal);
        /// Records the colour a producer rendered for this camera; `gamma` when gamma-encoded.
        void publishSceneColorMap(const std::shared_ptr<Texture>& texture, bool gamma);

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
        float _aperture = 16.0f;
        float _shutter = 1.0f / 1000.0f;
        float _sensitivity = 1000.0f;
        ClipPlanes _clip;

        uint32_t _cullingMask = 0xFFFFFFFFu;
        bool _skyVisible = true;
        float _exposureScale = 1.0f;
        float _orthoHeight = 10.0f;

        ProjectionType _projection = ProjectionType::Perspective;

        // Off-center projection offset, in half-frustum units. See setProjectionOffset().
        Vector2 _projectionOffset;

        bool _horizontalFov = false;

        CalculateProjection _calculateProjection;

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
        std::vector<std::shared_ptr<RenderPass>> _afterPasses;

        std::shared_ptr<RenderTarget> _renderTarget;

        Color _clearColor = Color(0.75f, 0.75f, 0.75f, 1.0f);
        std::array<std::optional<Color>, 3> _attachmentClearColors;
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

        std::shared_ptr<SceneDepthMapHandle> _sceneDepthMapHandle = std::make_shared<SceneDepthMapHandle>();
        std::shared_ptr<SceneColorMapHandle> _sceneColorMapHandle = std::make_shared<SceneColorMapHandle>();

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

        // Lightmap bake pass. Meshes drawn by
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
