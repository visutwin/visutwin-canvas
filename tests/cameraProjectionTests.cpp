// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#include <cmath>
#include <iostream>

#include "scene/camera.h"
// Camera holds a unique_ptr<GraphNode>, so its (implicit) constructor and destructor need the
// complete type.
#include "scene/graphNode.h"

using namespace visutwin::canvas;

namespace
{
    constexpr float EPSILON = 1e-4f;

    bool nearlyEqual(const float a, const float b)
    {
        return std::abs(a - b) <= EPSILON;
    }

    // Projects a view-space point through the matrix and returns normalized device coordinates.
    // The engine builds GL-convention projection matrices (clip.z is remapped to [0, 1] later, in
    // the vertex shader), so the x/y perspective divide here matches what the shader sees.
    void projectToNdc(const Matrix4& projection, const Vector3& viewPoint, float& outX, float& outY)
    {
        float clip[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        const float v[4] = {viewPoint.getX(), viewPoint.getY(), viewPoint.getZ(), 1.0f};
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int col = 0; col < 4; ++col) {
                sum += projection.getElement(col, row) * v[col];
            }
            clip[row] = sum;
        }
        outX = clip[0] / clip[3];
        outY = clip[1] / clip[3];
    }

    bool checkPerspective()
    {
        Camera camera;
        camera.setProjection(ProjectionType::Perspective);
        camera.setFov(45.0f);
        camera.setAspectRatio(1.0f);
        camera.setNearClip(0.1f);
        camera.setFarClip(100.0f);

        if (!nearlyEqual(camera.projectionOffset().x, 0.0f) ||
            !nearlyEqual(camera.projectionOffset().y, 0.0f)) {
            std::cerr << "FAILED: projection offset should default to (0, 0)\n";
            return false;
        }

        // A point straight ahead, in front of the camera (the engine looks down -Z).
        const Vector3 point(0.0f, 0.0f, -10.0f);

        float baseX = 0.0f, baseY = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, baseX, baseY);
        if (!nearlyEqual(baseX, 0.0f) || !nearlyEqual(baseY, 0.0f)) {
            std::cerr << "FAILED: unshifted camera should project a centred point to NDC origin, got "
                      << baseX << ", " << baseY << "\n";
            return false;
        }

        // An offset of (0, 1) moves the projection window up by half the frustum height, so
        // content shifts down by one NDC unit (NDC spans [-1, 1], i.e. half the viewport).
        camera.setProjectionOffset(Vector2(0.0f, 1.0f));
        float shiftedX = 0.0f, shiftedY = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, shiftedX, shiftedY);
        if (!nearlyEqual(shiftedX, 0.0f) || !nearlyEqual(shiftedY, -1.0f)) {
            std::cerr << "FAILED: perspective offset (0, 1) should give NDC (0, -1), got "
                      << shiftedX << ", " << shiftedY << "\n";
            return false;
        }

        // The horizontal axis uses the same convention.
        camera.setProjectionOffset(Vector2(0.5f, 0.0f));
        projectToNdc(camera.projectionMatrix(), point, shiftedX, shiftedY);
        if (!nearlyEqual(shiftedX, -0.5f) || !nearlyEqual(shiftedY, 0.0f)) {
            std::cerr << "FAILED: perspective offset (0.5, 0) should give NDC (-0.5, 0), got "
                      << shiftedX << ", " << shiftedY << "\n";
            return false;
        }

        // The shift-lens identity: to frame content `pitch` degrees above the horizon without
        // tilting the camera, the offset is tan(pitch) / tan(fovY / 2). That must land the
        // off-axis point exactly where an untilted, unshifted camera would have put the horizon.
        const float degToRad = std::acos(-1.0f) / 180.0f;
        const float pitch = 10.0f;
        const float fovY = 45.0f * degToRad;
        const float shift = std::tan(pitch * degToRad) / std::tan(fovY * 0.5f);
        camera.setProjectionOffset(Vector2(0.0f, shift));

        // A point 10 degrees above the view axis, at distance 10 along -Z.
        const Vector3 elevated(0.0f, 10.0f * std::tan(pitch * degToRad), -10.0f);
        projectToNdc(camera.projectionMatrix(), elevated, shiftedX, shiftedY);
        if (!nearlyEqual(shiftedY, 0.0f)) {
            std::cerr << "FAILED: shift-lens offset should centre the elevated point, got y = "
                      << shiftedY << "\n";
            return false;
        }

        return true;
    }

    bool checkOrthographic()
    {
        Camera camera;
        camera.setProjection(ProjectionType::Orthographic);
        camera.setAspectRatio(1.0f);
        camera.setOrthoHeight(10.0f);
        camera.setNearClip(0.1f);
        camera.setFarClip(100.0f);

        const Vector3 point(0.0f, 0.0f, -10.0f);

        float baseX = 0.0f, baseY = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, baseX, baseY);
        if (!nearlyEqual(baseX, 0.0f) || !nearlyEqual(baseY, 0.0f)) {
            std::cerr << "FAILED: unshifted ortho camera should project to NDC origin, got "
                      << baseX << ", " << baseY << "\n";
            return false;
        }

        // Same sign convention as the perspective path: offset (0, 1) shifts content down by
        // one NDC unit, which for ortho is half the projection window height.
        camera.setProjectionOffset(Vector2(0.0f, 1.0f));
        float shiftedX = 0.0f, shiftedY = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, shiftedX, shiftedY);
        if (!nearlyEqual(shiftedX, 0.0f) || !nearlyEqual(shiftedY, -1.0f)) {
            std::cerr << "FAILED: ortho offset (0, 1) should give NDC (0, -1), got "
                      << shiftedX << ", " << shiftedY << "\n";
            return false;
        }

        return true;
    }

    // Every property feeding evaluateProjectionMatrix() must invalidate the cached matrix.
    bool checkProjectionInvalidation()
    {
        Camera camera;
        camera.setProjection(ProjectionType::Perspective);
        camera.setFov(45.0f);
        camera.setAspectRatio(1.0f);
        camera.setNearClip(0.1f);
        camera.setFarClip(100.0f);

        // A point off the view axis, so a narrower field of view pushes it further out.
        const Vector3 point(1.0f, 0.0f, -10.0f);

        float wideX = 0.0f, wideY = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, wideX, wideY);

        camera.setFov(20.0f);
        float narrowX = 0.0f, narrowY = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, narrowX, narrowY);

        if (nearlyEqual(narrowX, wideX)) {
            std::cerr << "FAILED: changing fov did not rebuild the projection matrix (stale at x = "
                      << wideX << ")\n";
            return false;
        }
        if (narrowX <= wideX) {
            std::cerr << "FAILED: narrowing fov should push an off-axis point outwards, got "
                      << wideX << " -> " << narrowX << "\n";
            return false;
        }

        // The remaining projection inputs, for completeness.
        camera.setNearClip(1.0f);
        float x = 0.0f, y = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, x, y);

        camera.setAspectRatio(2.0f);
        float aspectX = 0.0f, aspectY = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, aspectX, aspectY);
        if (nearlyEqual(aspectX, x)) {
            std::cerr << "FAILED: changing aspect ratio did not rebuild the projection matrix\n";
            return false;
        }

        camera.setProjection(ProjectionType::Orthographic);
        camera.setOrthoHeight(10.0f);
        float orthoX = 0.0f, orthoY = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, orthoX, orthoY);

        camera.setOrthoHeight(5.0f);
        float orthoX2 = 0.0f, orthoY2 = 0.0f;
        projectToNdc(camera.projectionMatrix(), point, orthoX2, orthoY2);
        if (nearlyEqual(orthoX2, orthoX)) {
            std::cerr << "FAILED: changing ortho height did not rebuild the projection matrix\n";
            return false;
        }

        return true;
    }

    bool checkFrustumCorners()
    {
        Camera camera;
        camera.setProjection(ProjectionType::Perspective);
        camera.setFov(45.0f);
        camera.setAspectRatio(1.0f);

        const auto base = camera.getFrustumCorners(1.0f, 10.0f);

        // Offsetting by one half-frustum height must displace every corner by exactly the
        // half-extent at its own depth slice, leaving the frustum's size unchanged. Directional
        // shadow cascades are fitted to these corners, so they have to track the shift.
        camera.setProjectionOffset(Vector2(0.0f, 1.0f));
        const auto shifted = camera.getFrustumCorners(1.0f, 10.0f);

        for (size_t i = 0; i < base.size(); ++i) {
            const bool isNear = i < 4;
            // Half-height at this slice: |y| of the corresponding unshifted corner.
            const float halfHeight = std::abs(base[i].getY());
            const float expectedY = base[i].getY() + halfHeight;
            if (!nearlyEqual(shifted[i].getY(), expectedY) ||
                !nearlyEqual(shifted[i].getX(), base[i].getX()) ||
                !nearlyEqual(shifted[i].getZ(), base[i].getZ())) {
                std::cerr << "FAILED: " << (isNear ? "near" : "far") << " frustum corner " << i
                          << " not displaced correctly, expected y = " << expectedY
                          << ", got " << shifted[i].getY() << "\n";
                return false;
            }
        }

        return true;
    }
}

int main()
{
    if (!checkPerspective() || !checkOrthographic() || !checkFrustumCorners() ||
        !checkProjectionInvalidation()) {
        return 1;
    }

    std::cout << "camera projection tests passed\n";
    return 0;
}
