// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Light culling turns geometry into visibility, and both halves are invisible in a
// render until they are wrong in a way that removes a light — at which point the
// question "is that light culled or is the shading broken" is expensive to answer.
// So the geometry is pinned here instead.
//
// The property that matters is CONTAINMENT: whatever the bounding volume is spelled
// as, every point the light can reach must be inside it, or culling removes a light
// that was lighting something. A spot's cone is the interesting case, because it is
// the one where a tight bound is worth having and the one where a sign error puts
// the sphere behind the light — which culls it exactly when it is pointing at you.
//
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <vector>

#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "scene/camera.h"
#include "scene/frustumUtils.h"
#include "scene/graphNode.h"
#include "scene/light.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }

    void checkClose(const float actual, const float expected, const char* what,
        const float tolerance = 1e-4f)
    {
        if (std::fabs(actual - expected) > tolerance) {
            std::printf("FAIL: %s — expected %.6f, got %.6f\n", what, expected, actual);
            ++failures;
        }
    }

    constexpr float kDegToRad = std::numbers::pi_v<float> / 180.0f;

    /// A light on its own node, positioned and aimed. `pitchDegrees` tilts the node
    /// about X: at 0 the light shines down -Y, its rest direction.
    struct TestLight
    {
        GraphNode node;
        Light light{nullptr, false};

        TestLight(const LightType type, const Vector3& position, const float pitchDegrees,
            const float range, const float outerConeAngle)
        {
            node.setLocalPosition(position);
            node.setLocalRotation(Quaternion::fromEulerAngles(pitchDegrees, 0.0f, 0.0f));
            light.setType(type);
            light.setRange(range);
            light.setOuterConeAngle(outerConeAngle);
            light.setNode(&node);
        }
    };

    /// Points on the surface of a spot's cone: the apex, and a ring at the far rim.
    /// Every one of them is lit, so every one has to be inside the bound.
    std::vector<Vector3> conePoints(const Vector3& apex, const Vector3& axis,
        const float range, const float outerConeAngle)
    {
        // Any two directions perpendicular to the axis span the rim.
        Vector3 side = std::fabs(axis.getY()) < 0.9f
            ? Vector3(0.0f, 1.0f, 0.0f) : Vector3(1.0f, 0.0f, 0.0f);
        side = axis.cross(side).normalized();
        const Vector3 other = axis.cross(side).normalized();

        const float outer = outerConeAngle * kDegToRad;
        const Vector3 rimCentre = apex + axis * (range * std::cos(outer));
        const float rimRadius = range * std::sin(outer);

        std::vector<Vector3> points{apex};
        for (int i = 0; i < 16; ++i) {
            const float t = static_cast<float>(i) / 16.0f * 2.0f
                * std::numbers::pi_v<float>;
            points.push_back(rimCentre
                + side * (rimRadius * std::cos(t))
                + other * (rimRadius * std::sin(t)));
        }
        return points;
    }

    void checkContainsCone(TestLight& testLight, const Vector3& axis,
        const float range, const float outerConeAngle, const char* what)
    {
        const BoundingSphere bounds = testLight.light.boundingSphere();
        const Vector3 apex(testLight.node.worldTransform().getColumn(3));
        for (const auto& point : conePoints(apex, axis, range, outerConeAngle)) {
            const float distance = (point - bounds.center()).length();
            if (distance > bounds.radius() + 1e-3f) {
                std::printf("FAIL: %s — a lit point is %.4f from the centre, "
                            "outside a radius of %.4f\n", what, distance, bounds.radius());
                ++failures;
                return;
            }
        }
    }
}

int main()
{
    // ── An omni light is bounded by its range ────────────────────────────────
    {
        const TestLight omni(LightType::LIGHTTYPE_OMNI,
            Vector3(2.0f, 3.0f, -4.0f), 0.0f, 7.0f, 45.0f);
        const BoundingSphere bounds = omni.light.boundingSphere();
        checkClose(bounds.radius(), 7.0f, "omni radius is its range");
        checkClose(bounds.center().getX(), 2.0f, "omni centre x");
        checkClose(bounds.center().getY(), 3.0f, "omni centre y");
        checkClose(bounds.center().getZ(), -4.0f, "omni centre z");
    }

    // ── A spot aims down -Y at rest, and its bound follows the cone ──────────
    // Rest direction, so the bound must sit BELOW the light. A sign error here
    // puts it above and culls the light whenever what it lights is on screen.
    {
        const float range = 10.0f, outer = 20.0f;   // narrow: the <= 45 branch
        TestLight spot(LightType::LIGHTTYPE_SPOT,
            Vector3(0.0f, 5.0f, 0.0f), 0.0f, range, outer);
        const BoundingSphere bounds = spot.light.boundingSphere();

        const float expectedRadius = range / (2.0f * std::cos(outer * kDegToRad));
        checkClose(bounds.radius(), expectedRadius, "narrow spot radius");
        checkClose(bounds.center().getY(), 5.0f - expectedRadius,
            "narrow spot centre is BELOW the light, along its beam");
        checkClose(bounds.center().getX(), 0.0f, "narrow spot centre x");

        checkContainsCone(spot, Vector3(0.0f, -1.0f, 0.0f), range, outer,
            "narrow spot bound contains its cone");

        // The whole point of the cone bound: it is much smaller than the range
        // sphere an omni of the same reach would need.
        check(bounds.radius() < range,
            "narrow spot is bounded more tightly than its range");
    }

    // ── A wide spot takes the other branch, and still contains its cone ──────
    {
        const float range = 10.0f, outer = 70.0f;   // wide: the > 45 branch
        TestLight spot(LightType::LIGHTTYPE_SPOT,
            Vector3(0.0f, 5.0f, 0.0f), 0.0f, range, outer);
        const BoundingSphere bounds = spot.light.boundingSphere();

        checkClose(bounds.radius(), range * std::sin(outer * kDegToRad),
            "wide spot radius");
        checkContainsCone(spot, Vector3(0.0f, -1.0f, 0.0f), range, outer,
            "wide spot bound contains its cone");
    }

    // ── Rotating the node turns the beam, and the bound goes with it ─────────
    // Pitching by 90 degrees about X takes the beam from -Y to -Z. If the bound
    // ignored the node's rotation this would still look fine in any scene whose
    // spots point down, which is most of them.
    {
        const float range = 10.0f, outer = 20.0f;
        TestLight spot(LightType::LIGHTTYPE_SPOT,
            Vector3(0.0f, 0.0f, 0.0f), 90.0f, range, outer);
        const BoundingSphere bounds = spot.light.boundingSphere();

        const float expectedRadius = range / (2.0f * std::cos(outer * kDegToRad));
        checkClose(bounds.center().getY(), 0.0f, "pitched spot no longer reaches down");
        checkClose(bounds.center().getZ(), -expectedRadius,
            "pitched spot reaches along -Z");

        checkContainsCone(spot, Vector3(0.0f, 0.0f, -1.0f), range, outer,
            "pitched spot bound contains its cone");
    }

    // ── Camera::screenSize ranks lights the way the 8 slots need ─────────────
    {
        GraphNode cameraNode;
        cameraNode.setLocalPosition(Vector3(0.0f, 0.0f, 0.0f));
        Camera camera;
        camera.setNode(&cameraNode);
        camera.setFov(60.0f);

        const BoundingSphere near(Vector3(0.0f, 0.0f, -5.0f), 1.0f);
        const BoundingSphere far(Vector3(0.0f, 0.0f, -50.0f), 1.0f);
        const BoundingSphere big(Vector3(0.0f, 0.0f, -50.0f), 10.0f);

        check(camera.screenSize(near) > camera.screenSize(far),
            "a nearer light of the same size covers more of the screen");
        check(camera.screenSize(big) > camera.screenSize(far),
            "a bigger light at the same distance covers more of the screen");
        checkClose(camera.screenSize(BoundingSphere(Vector3(0.0f), 5.0f)), 1.0f,
            "a light the camera is inside fills the view");
        check(camera.screenSize(far) > 0.0f, "a distant light still counts for something");
    }

    // ── The decision itself: in front passes, behind and beside do not ───────
    // The geometry above is only half of it. This is the half that removes a light,
    // and it has to remove the right ones — the examples in the tree all keep every
    // light on screen, so nothing there exercises a culled light at all.
    {
        GraphNode cameraNode;   // at the origin, unrotated: looking down -Z
        Camera camera;
        camera.setNode(&cameraNode);
        camera.setFov(45.0f);
        camera.setAspectRatio(1.0f);
        camera.setNearClip(0.1f);
        camera.setFarClip(100.0f);
        const Frustum frustum = buildCameraFrustum(&camera, &cameraNode);

        const auto reaches = [&](const Vector3& position, const float range) {
            TestLight omni(LightType::LIGHTTYPE_OMNI, position, 0.0f, range, 45.0f);
            const BoundingSphere bounds = omni.light.boundingSphere();
            return frustum.checkSphere(bounds.center(), bounds.radius());
        };

        check(reaches(Vector3(0.0f, 0.0f, -10.0f), 1.0f),
            "a light in front of the camera survives the cull");
        check(!reaches(Vector3(0.0f, 0.0f, 10.0f), 1.0f),
            "a light BEHIND the camera is culled");
        check(!reaches(Vector3(100.0f, 0.0f, -10.0f), 1.0f),
            "a light far to the side is culled");
        check(!reaches(Vector3(0.0f, 0.0f, -500.0f), 1.0f),
            "a light past the far plane is culled");

        // Range is part of the decision, not just position: a light whose reach
        // crosses into view has to survive even though its centre does not.
        check(!reaches(Vector3(0.0f, 0.0f, 10.0f), 5.0f),
            "a short-ranged light behind the camera stays culled");
        check(reaches(Vector3(0.0f, 0.0f, 10.0f), 30.0f),
            "a light behind the camera whose RANGE reaches into view survives");
    }

    if (failures == 0) {
        std::printf("light culling: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
