// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2025.
//
#pragma once

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
         * World → cookie-UV projection for a spot light's cookie (upstream
         * LightCamera.evalSpotCookieMatrix). Shadow-casting spots already have the
         * same matrix as their shadow VP; this evaluates it for cookie-only lights,
         * which never get a shadow camera.
         */
        static Matrix4 evalSpotCookieMatrix(const Light& light);

        /** NDC → texture-UV bias matrix shared by spot shadow and cookie projections. */
        static Matrix4 spotProjectionBias();
    };
}
