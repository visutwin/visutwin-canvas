// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2025.
//
#pragma once

#include "scene/camera.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class LightCamera
    {
    public:
        // Camera rotation angles used when rendering cubemap faces
        static Quaternion pointLightRotations[6];

        static Camera* create(const std::string& name, LightType lightType, int face = 0);
    };
}
