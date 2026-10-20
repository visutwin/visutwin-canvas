// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2025
//
#pragma once

#include "core/math/vector4.h"

#include "scene/camera.h"
#include "scene/constants.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    class LightCamera
    {
    public:
        // Camera rotation angles used when rendering cubemap faces
        static Quaternion pointLightRotations[6];

        static Camera* create(const std::string& name, LightType lightType, int face = 0);

        /**
         * World → cookie-UV projection for a spot light's cookie. Shadow-casting spots already have the
         * same matrix as their shadow VP; this evaluates it for cookie-only lights,
         * which never get a shadow camera.
         */
        static Matrix4 evalSpotCookieMatrix(const Light& light);
        /// The same projection into `viewport` of a texture (a clustered light's rect of
        /// the cookie atlas) instead of the whole of it.
        static Matrix4 evalSpotCookieMatrix(const Light& light, const Vector4& viewport);

        /// World -> UV of the cookie TEXTURE itself, for a cookie sampled directly (the
        /// non-clustered path). The shadow projections above flip Y, because a shadow map is
        /// rendered with its origin at the top; a cookie is an image, whose v runs the other way
        /// along the light camera's up axis, so this undoes the flip (v -> 1 - v), shadowed or not.
        static Matrix4 evalSpotCookieTextureMatrix(const Light& light);

        /** NDC → texture-UV bias matrix shared by spot shadow and cookie projections. */
        static Matrix4 spotProjectionBias();

        /// NDC -> texture UV within `viewport` (normalized x, y, width, height, origin
        /// top-left), z [-1,1] -> [0,1]: the bias a spot's shadow or cookie projection
        /// needs when its map is a rect of the clustered atlas rather than a whole
        /// texture. `spotProjectionBias()` is this for the full texture.
        static Matrix4 viewportProjectionBias(const Vector4& viewport);
    };
}
