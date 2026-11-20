// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#pragma once

#include <memory>

#include "core/math/matrix4.h"
#include "core/math/vector4.h"

namespace visutwin::canvas
{
    class RenderTarget;

    /**
     * One of the views a camera renders in a frame, when it renders more than one: an eye of
     * an XR session, or a depth slice of one. The camera's layers are drawn once per view,
     * each with the view's own matrices into the view's own target, while the camera is
     * culled, sorted and shadowed once, for the volume all of its views cover.
     */
    struct CameraView
    {
        /// The view's projection, in the engine's GL-style clip space (z in [-1, 1]).
        Matrix4 projection = Matrix4::identity();

        /// World to view, and its inverse (the view's pose in the world).
        Matrix4 view = Matrix4::identity();
        Matrix4 viewInverse = Matrix4::identity();

        /// The rectangle of `renderTarget` the view draws into, in pixels: x, y, width,
        /// height, with the origin at the target's top-left corner.
        Vector4 viewport{0.0f, 0.0f, 1.0f, 1.0f};

        /// Whether the view holds the background: the camera's clear colour and the sky. A
        /// view without it (every depth slice but the farthest) clears to transparent black
        /// and draws no sky, so it can be laid over the views behind it.
        bool background = true;

        /// Where the view is drawn. Views do not share a target: each one's pass clears its own.
        std::shared_ptr<RenderTarget> renderTarget;
    };
}
