// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2025.
//
#include "lightCamera.h"

#include "scene/graphNode.h"

namespace visutwin::canvas
{
    Quaternion LightCamera::pointLightRotations[6] = {
        Quaternion::fromEulerAngles(0, 90, 180),
        Quaternion::fromEulerAngles(0, -90, 180),
        Quaternion::fromEulerAngles(90, 0, 0),
        Quaternion::fromEulerAngles(-90, 0, 0),
        Quaternion::fromEulerAngles(0, 180, 180),
        Quaternion::fromEulerAngles(0, 0, 180)
    };

    Camera* LightCamera::create(const std::string& name, LightType lightType, int face)
    {
        Camera* camera = new Camera();
        camera->setNode(new GraphNode(name));
        camera->setAspectRatio(1.0f);
        camera->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
        camera->setScissorRectClear(true);

        // Set up constant settings based on a light type
        switch (lightType) {
        case LightType::LIGHTTYPE_OMNI:
        case LightType::LIGHTTYPE_POINT:
            camera->node()->setRotation(pointLightRotations[face]);
            camera->setFov(90.0f);
            camera->setProjection(ProjectionType::Perspective);
            break;

        case LightType::LIGHTTYPE_SPOT:
        case LightType::LIGHTTYPE_AREA_RECT:
            camera->setProjection(ProjectionType::Perspective);
            break;

        case LightType::LIGHTTYPE_DIRECTIONAL:
            camera->setProjection(ProjectionType::Orthographic);
            break;
        }

        return camera;
    }
}
