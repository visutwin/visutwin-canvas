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

    Matrix4 LightCamera::evalSpotCookieMatrix(const Light& light)
    {
        // position()/rotation() lazily resolve the world transform and so are
        // non-const on GraphNode.
        GraphNode* node = light.node();
        if (!node) {
            return Matrix4::identity();
        }

        // A camera at the light, looking down the beam. The camera looks along its
        // -Z while a spot light emits along -Y, hence the local -90° X rotation
        // (upstream evalSpotCookieMatrix does the same rotateLocal(-90, 0, 0)).
        // Cached like upstream's _spotCookieCamera — this runs per cookie light
        // per frame and the camera holds no per-light state between calls.
        static const std::unique_ptr<Camera> cookieCamera = [] {
            auto camera = std::unique_ptr<Camera>(create("SpotCookieCamera", LightType::LIGHTTYPE_SPOT));
            return camera;
        }();

        cookieCamera->setFov(std::min(light.outerConeAngle() * 2.0f, 179.0f));
        cookieCamera->setNearClip(0.01f);
        cookieCamera->setFarClip(std::max(light.range(), 0.1f));
        cookieCamera->node()->setPosition(node->position());
        cookieCamera->node()->setRotation(
            node->rotation() * Quaternion::fromEulerAngles(-90.0f, 0.0f, 0.0f));

        const Matrix4 viewProj = cookieCamera->projectionMatrix()
            * cookieCamera->node()->worldTransform().inverse();
        return spotProjectionBias() * viewProj;
    }

    Matrix4 LightCamera::spotProjectionBias()
    {
        // NDC → texture UV: x,y [-1,1] → [0,1] with Y flipped (texture origin is
        // top-left in both backends), z [-1,1] → [0,1] to match the shadow vertex
        // shader's clip.z = 0.5 * (clip.z + clip.w) remap.
        Matrix4 bias = Matrix4::identity();
        bias.setElement(0, 0, 0.5f);
        bias.setElement(3, 0, 0.5f);
        bias.setElement(1, 1, -0.5f);
        bias.setElement(3, 1, 0.5f);
        bias.setElement(2, 2, 0.5f);
        bias.setElement(3, 2, 0.5f);
        return bias;
    }

    Camera* LightCamera::create(const std::string& name, LightType lightType, int face)
    {
        Camera* camera = new Camera();
        camera->setOwnedNode(std::make_unique<GraphNode>(name));
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
