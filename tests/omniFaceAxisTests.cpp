// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The one-pass omni shadow caster classification
// (scene/renderer/omniShadowCasterClassification.cpp) tests a caster's AABB against
// all six cube faces at once, using the fact that each face looks straight down a
// world axis in the fixed order +X, -X, +Y, -Y, +Z, -Z. That is a property of
// LightCamera::pointLightRotations, not of the classification, so it cannot be
// checked where it is relied on. Nothing about the rotation table announces the
// order either: it is six Euler triples, and reordering them or changing a sign
// would leave every shadow map still rendering, just with casters assigned to the
// wrong faces — a silent, partial loss of shadows.
//
// So it is checked here. If this test fails, do NOT adjust it to match: either put
// the rotation table back, or rewrite the classification's six blocks to the new
// order.

#include <cmath>
#include <memory>
#include <iostream>

#include "scene/camera.h"
#include "scene/graphNode.h"
#include "scene/renderer/lightCamera.h"
#include "scene/renderer/shadowRenderer.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float EPSILON = 1e-4f;

    // A camera looks down its own -Z.
    Vector3 cameraForward(const Quaternion& rotation)
    {
        GraphNode node("probe");
        node.setLocalRotation(rotation);
        const Matrix4 world = node.worldTransform();
        return Vector3(-world.getElement(2, 0), -world.getElement(2, 1),
            -world.getElement(2, 2));
    }

    bool checkFaceAxes()
    {
        const Vector3 expected[6] = {
            Vector3(1.0f, 0.0f, 0.0f), Vector3(-1.0f, 0.0f, 0.0f),
            Vector3(0.0f, 1.0f, 0.0f), Vector3(0.0f, -1.0f, 0.0f),
            Vector3(0.0f, 0.0f, 1.0f), Vector3(0.0f, 0.0f, -1.0f),
        };
        const char* names[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};

        bool ok = true;
        for (int face = 0; face < 6; ++face) {
            const Vector3 forward = cameraForward(LightCamera::pointLightRotations[face]);
            const bool matches =
                std::abs(forward.getX() - expected[face].getX()) < EPSILON &&
                std::abs(forward.getY() - expected[face].getY()) < EPSILON &&
                std::abs(forward.getZ() - expected[face].getZ()) < EPSILON;
            if (!matches) {
                std::cerr << "omni shadow face " << face << " should look down "
                          << names[face] << " but looks down (" << forward.getX() << ", "
                          << forward.getY() << ", " << forward.getZ() << ")\n";
                ok = false;
            }
        }
        return ok;
    }

    // The classification also assumes the faces cover the sphere without a gap, which
    // needs a field of view of at least 90 degrees. A narrower one would leave wedges
    // between the faces that a caster could sit in and be dropped from all six lists.
    bool checkFaceFov()
    {
        const std::unique_ptr<Camera> camera =
            ShadowRenderer::createShadowCamera(SHADOW_PCF3_32F, LightType::LIGHTTYPE_OMNI, 0);
        if (!camera) {
            std::cerr << "no omni shadow camera\n";
            return false;
        }
        if (camera->fov() < 90.0f - EPSILON) {
            std::cerr << "omni shadow face fov is " << camera->fov()
                      << ", below the 90 degrees the six faces need to cover the sphere\n";
            return false;
        }
        return true;
    }
}

int main()
{
    const bool ok = checkFaceAxes() && checkFaceFov();
    if (!ok) {
        std::cerr << "omni face axis tests FAILED\n";
        return 1;
    }
    std::cout << "omni face axis tests passed\n";
    return 0;
}
