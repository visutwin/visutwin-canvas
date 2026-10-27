// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// A camera's handles to its scene depth and colour maps. The device's grab maps
// (GraphicsDevice::sceneColorMap / sceneDepthGrabMap) are shared by every camera and hold
// whichever map was published last; a handle belongs to ONE camera, is the same object for the
// camera's whole life, and names the map that camera rendered most recently, together with how
// it is stored, even as the camera renders it to a different texture. A compute kernel reads a
// camera's maps through them (Compute::setSceneDepthMap / setSceneColorMap).
//
// The texture is held WEAKLY: the producer (a grab pass, a camera frame) owns it and may free
// it at any option change, and a handle may outlive both the texture and the camera (a Compute
// keeps it). A texture that is gone reads as no map, as a destroyed camera does.
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "core/math/matrix4.h"

namespace visutwin::canvas
{
    class Texture;

    /// The scene depth map a camera rendered most recently (Camera::sceneDepthMapHandle).
    struct SceneDepthMapHandle
    {
        /// The texture the depth was rendered to; expired when the camera has rendered none, or
        /// its producer freed it.
        std::weak_ptr<Texture> texture;

        /// The render version it was rendered in, so a consumer can tell this frame's map from
        /// one left over from an earlier frame.
        int renderVersion = -1;

        /// True when the texture stores the linear camera depth, false when it stores the depth
        /// buffer's values, which a consumer linearises with cameraParams. Every producer in
        /// this engine stores depth buffer values; the flags are kept for a linear producer.
        bool linear = false;

        /// Only with `linear`: the depth bit-packed into an RGBA8 texel (not supported by the
        /// compute module), or a coverage-weighted average of the reciprocal depths.
        bool packed = false;
        bool reciprocal = false;

        /// The camera parameters the depth was rendered with: 1 / far clip, far clip, near clip,
        /// and 1 for an orthographic projection or 0 for a perspective one. Captured at
        /// publication, so a consumer decodes the map correctly after the camera changes.
        std::array<float, 4> cameraParams = {0.0f, 0.0f, 0.0f, 0.0f};

        /// The inverse of the view-projection the shaders were given when the depth was
        /// rendered (jitter included), GL clip conventions (NDC z in [-1, 1]).
        Matrix4 viewProjectionInverse = Matrix4::identity();

        /// The viewport the camera rendered the depth with, in texels as a compute kernel
        /// addresses them (row 0 at the top): x and y of its first texel, width and height.
        /// Outside it the map holds what the camera did not render.
        std::array<uint32_t, 4> viewport = {0u, 0u, 0u, 0u};

        /// True once the camera the handle belongs to has been destroyed.
        bool destroyed = false;

        /// The size of the map in texels, 0 while there is none. Under a render scale or a
        /// render target it is not the canvas size, so a dispatch over the map is sized by it.
        uint32_t width() const;
        uint32_t height() const;
    };

    /// The scene colour map a camera rendered most recently (Camera::sceneColorMapHandle).
    struct SceneColorMapHandle
    {
        /// The texture the colour was rendered to; expired when there is none (see above).
        std::weak_ptr<Texture> texture;

        /// True when the texture stores gamma-encoded colours (a camera rendering straight to
        /// its target), false when linear (a camera frame's HDR scene).
        bool gamma = false;

        /// True once the camera the handle belongs to has been destroyed.
        bool destroyed = false;

        /// The size of the map in texels, 0 while there is none (see the depth handle).
        uint32_t width() const;
        uint32_t height() const;
    };
}
