// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"

namespace visutwin::canvas
{
    class RenderTarget;
    class Texture;

    /// What a session presents: a virtual world, or the world with the scene over it.
    enum class XrSessionType
    {
        Vr,
        Ar,
    };

    /// The space poses are reported in. Viewer follows the head; Local has its origin near
    /// the head at session start; LocalFloor the same with y = 0 on the floor; BoundedFloor a
    /// room-scale space with bounds; Unbounded a space the user may walk far in.
    enum class XrSpaceType
    {
        Viewer,
        Local,
        LocalFloor,
        BoundedFloor,
        Unbounded,
    };

    enum class XrEye
    {
        None,
        Left,
        Right,
    };

    enum class XrHandedness
    {
        None,
        Left,
        Right,
    };

    enum class XrTargetRayMode
    {
        Gaze,           ///< the ray follows the head
        TrackedPointer, ///< the ray comes from a handheld device or a hand
        Screen,         ///< the ray comes from a touch on a handheld screen
    };

    /// Whether the session's frames reach the user, and whether it has input focus.
    enum class XrVisibility
    {
        Hidden,
        VisibleBlurred,
        Visible,
    };

    struct XrPose
    {
        Vector3 position{0.0f, 0.0f, 0.0f};
        Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    };

    /// A view's field of view as four half-angles in radians from its forward axis: left and
    /// down are negative for a symmetric view.
    struct XrFov
    {
        float left = -0.785398f;
        float right = 0.785398f;
        float up = 0.785398f;
        float down = -0.785398f;
    };

    /// One view of the frame as its backend reports it: where it is in the reference space
    /// and what it sees.
    struct XrViewState
    {
        XrEye eye = XrEye::None;
        XrPose pose;
        XrFov fov;
    };

    /// Where one view (or one depth slice of it) is drawn this frame.
    struct XrViewTarget
    {
        std::shared_ptr<RenderTarget> target;
        /// x, y, width, height in pixels of the target, origin at its top-left corner.
        Vector4 viewport{0.0f, 0.0f, 1.0f, 1.0f};
    };

    /// When the frame being prepared will be shown, and whether drawing it is worth anything.
    struct XrFrameTiming
    {
        bool shouldRender = true;
        /// Seconds on the backend's own clock.
        double predictedDisplayTime = 0.0;
        double predictedDisplayPeriod = 0.0;
    };

    /// An input source of the frame as its backend reports it. Poses are in the reference space.
    struct XrInputSourceState
    {
        /// Stable for as long as the source exists.
        uint32_t id = 0;
        XrHandedness handedness = XrHandedness::None;
        XrTargetRayMode targetRayMode = XrTargetRayMode::TrackedPointer;
        /// Interaction profiles, most specific first ("oculus-touch", "generic-trigger-squeeze").
        std::vector<std::string> profiles;

        bool gripValid = false;
        XrPose grip;
        bool velocityValid = false;
        Vector3 linearVelocity{0.0f, 0.0f, 0.0f};

        bool rayValid = false;
        XrPose ray;

        /// The primary action (trigger) and the secondary one (grip squeeze), with their
        /// analogue values in [0, 1].
        bool select = false;
        bool squeeze = false;
        float selectValue = 0.0f;
        float squeezeValue = 0.0f;

        /// Further controls in the layout of a standard XR gamepad: buttons 0 trigger,
        /// 1 squeeze, 2 touchpad, 3 thumbstick, 4 and 5 the face buttons; axes 0 and 1 the
        /// touchpad, 2 and 3 the thumbstick (x right, y TOWARD the user positive).
        std::vector<float> buttons;
        std::vector<float> axes;
    };

    /// What a session is started with.
    struct XrSessionOptions
    {
        XrSessionType type = XrSessionType::Vr;
        XrSpaceType spaceType = XrSpaceType::LocalFloor;

        /// Scales the size the backend recommends for a view's target.
        float framebufferScaleFactor = 1.0f;

        /// Multisampling of the views' targets (1: none).
        int samples = 4;

        /// How many depth slices each view is rendered as (1: the view as a whole). With more
        /// than one, the camera's depth range is cut at `depthSliceBoundaries` and every
        /// slice of every view is rendered into a target of its own, with only the geometry
        /// between its two distances, over a transparent clear: a display with several focal
        /// planes shows each slice on one of them, any other composites them far to near.
        int depthSlices = 1;

        /// The distances the slices are cut at, nearest first: depthSlices + 1 values from the
        /// camera's near clip to its far clip. Empty: xrDepthSliceBoundaries (even steps in
        /// dioptres, which is how focal planes are spaced).
        std::vector<float> depthSliceBoundaries;

        /// Copy the first view to the window each frame.
        bool mirror = true;
    };

    /// The frame as rendered, handed back to the backend for display.
    struct XrFrameSubmission
    {
        /// One per view: the pose and field of view its image was rendered with.
        std::vector<XrViewState> views;
        float nearClip = 0.1f;
        float farClip = 1000.0f;
        int depthSlices = 1;
        /// depthSlices + 1 distances, nearest first.
        std::vector<float> depthSliceBoundaries;
        bool rendered = false;
    };

    /// The GL-style projection (clip z in [-1, 1]) of a field of view between two distances.
    inline Matrix4 xrProjectionFromFov(const XrFov& fov, const float nearClip, const float farClip)
    {
        return Matrix4::frustum(
            nearClip * std::tan(fov.left), nearClip * std::tan(fov.right),
            nearClip * std::tan(fov.down), nearClip * std::tan(fov.up),
            nearClip, farClip);
    }

    /// `count + 1` distances from `nearClip` to `farClip` cutting the range into `count`
    /// slices of equal extent in dioptres (1 / distance), nearest first.
    inline std::vector<float> xrDepthSliceBoundaries(const float nearClip, const float farClip, const int count)
    {
        std::vector<float> boundaries;
        const int slices = count < 1 ? 1 : count;
        boundaries.reserve(static_cast<size_t>(slices) + 1);
        const float nearDioptres = 1.0f / nearClip;
        const float farDioptres = 1.0f / farClip;
        for (int i = 0; i <= slices; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(slices);
            boundaries.push_back(1.0f / (nearDioptres + (farDioptres - nearDioptres) * t));
        }
        boundaries.front() = nearClip;
        boundaries.back() = farClip;
        return boundaries;
    }
}
