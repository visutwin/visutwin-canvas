// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2026
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
// The faces are also rendered upside down against hardware cube addressing, which the
// shaders' cube shadow reads undo (omniShadowCubeDir); checkCubeAddressing holds the two.
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

    // Hardware cube addressing (Metal and Vulkan alike): the face of the major axis, and (s, t)
    // in [0, 1] with t = 0 the face's FIRST row.
    int hardwareCubeTexel(const Vector3& d, float& s, float& t)
    {
        const float ax = std::abs(d.getX()), ay = std::abs(d.getY()), az = std::abs(d.getZ());
        int face;
        float sc, tc, ma;
        if (ax >= ay && ax >= az) {
            ma = ax;
            face = d.getX() > 0.0f ? 0 : 1;
            sc = d.getX() > 0.0f ? -d.getZ() : d.getZ();
            tc = -d.getY();
        } else if (ay >= az) {
            ma = ay;
            face = d.getY() > 0.0f ? 2 : 3;
            sc = d.getX();
            tc = d.getY() > 0.0f ? d.getZ() : -d.getZ();
        } else {
            ma = az;
            face = d.getZ() > 0.0f ? 4 : 5;
            sc = d.getZ() > 0.0f ? d.getX() : -d.getX();
            tc = -d.getY();
        }
        s = (sc / ma + 1.0f) * 0.5f;
        t = (tc / ma + 1.0f) * 0.5f;
        return face;
    }

    // The shaders' omniShadowCubeDir (forward-fragment-head.slang), the reflection every cube
    // shadow read applies.
    Vector3 omniShadowCubeDir(const Vector3& d)
    {
        const float ax = std::abs(d.getX()), ay = std::abs(d.getY()), az = std::abs(d.getZ());
        if (ay > ax && ay >= az) {
            return Vector3(d.getX(), d.getY(), -d.getZ());
        }
        return Vector3(d.getX(), -d.getY(), d.getZ());
    }

    // Where a direction lands in its face as the face camera RENDERS it (row 0 the top of the
    // image, as every target here) against where the hardware looks it up. The faces come out
    // upside down against the hardware's t axis (u = s, v = 1 - t); the reflected lookup reads
    // the texel the direction was rendered at. A face table changed without the reflection
    // following it would leave every non-clustered omni shadow mirrored, which no frame of a
    // symmetric scene shows.
    bool checkCubeAddressing()
    {
        const std::unique_ptr<Camera> camera =
            ShadowRenderer::createShadowCamera(SHADOW_PCF3_32F, LightType::LIGHTTYPE_OMNI, 0);
        GraphNode node("face");
        camera->setNode(&node);
        camera->setFov(90.0f);
        camera->setAspectRatio(1.0f);
        const Vector3 directions[] = {
            Vector3(1.0f, 0.3f, 0.1f), Vector3(1.0f, -0.2f, 0.5f), Vector3(-1.0f, 0.3f, 0.1f),
            Vector3(-1.0f, -0.2f, 0.5f), Vector3(0.3f, 1.0f, 0.1f), Vector3(-0.2f, 1.0f, 0.5f),
            Vector3(0.3f, -1.0f, 0.1f), Vector3(-0.2f, -1.0f, 0.5f), Vector3(0.3f, 0.1f, 1.0f),
            Vector3(-0.2f, 0.5f, 1.0f), Vector3(0.3f, 0.1f, -1.0f), Vector3(-0.2f, 0.5f, -1.0f)};
        bool ok = true;
        for (const Vector3& d : directions) {
            float s = 0.0f, t = 0.0f;
            const int face = hardwareCubeTexel(d, s, t);
            node.setLocalRotation(LightCamera::pointLightRotations[face]);
            const Matrix4 viewProjection = camera->projectionMatrix() * node.worldTransform().inverse();
            const Vector4 clip = viewProjection * Vector4(d.getX(), d.getY(), d.getZ(), 1.0f);
            const float u = (clip.getX() / clip.getW() + 1.0f) * 0.5f;
            const float v = (1.0f - clip.getY() / clip.getW()) * 0.5f;
            if (std::abs(u - s) > EPSILON || std::abs(v - (1.0f - t)) > EPSILON) {
                std::cerr << "face " << face << " renders (" << d.getX() << ", " << d.getY() << ", " << d.getZ()
                          << ") at (" << u << ", " << v << "), not at the hardware's (s, 1 - t) = (" << s << ", "
                          << 1.0f - t << ")\n";
                ok = false;
            }
            float rs = 0.0f, rt = 0.0f;
            const int readFace = hardwareCubeTexel(omniShadowCubeDir(d), rs, rt);
            if (readFace != face || std::abs(rs - u) > EPSILON || std::abs(rt - v) > EPSILON) {
                std::cerr << "the reflected lookup of (" << d.getX() << ", " << d.getY() << ", " << d.getZ()
                          << ") reads face " << readFace << " at (" << rs << ", " << rt << "), not face " << face
                          << " at (" << u << ", " << v << ")\n";
                ok = false;
            }
        }
        return ok;
    }
}

int main()
{
    const bool ok = checkFaceAxes() && checkFaceFov() && checkCubeAddressing();
    if (!ok) {
        std::cerr << "omni face axis tests FAILED\n";
        return 1;
    }
    std::cout << "omni face axis tests passed\n";
    return 0;
}
