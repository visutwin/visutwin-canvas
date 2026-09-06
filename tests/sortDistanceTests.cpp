// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The forward pass sorts transparent draws back-to-front on the SIGNED depth
// along the camera's forward vector (scene/renderer/sortDistance.h), as upstream's
// layer.js does. It used to sort on the squared radial distance, which ranks an
// off-axis surface as farther than a centred one at the same view depth — by up
// to 1 / cos(fov / 2) — and cannot tell a surface behind the camera from one in
// front. This holds the contract with cases the radial form gets wrong.

#include <cmath>
#include <iostream>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include "scene/renderer/sortDistance.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    bool near(const float a, const float b, const float eps = 1e-4f)
    {
        return std::fabs(a - b) <= eps;
    }
}

int main()
{
    std::cout << "sort distance: signed view-axis depth\n";

    // A camera at the origin looking down -Z.
    const Vector3 camPos(0.0f, 0.0f, 0.0f);
    const Vector3 camFwd = cameraForwardOf(Matrix4::identity());
    check(near(camFwd.getX(), 0.0f) && near(camFwd.getY(), 0.0f) && near(camFwd.getZ(), -1.0f),
        "identity camera looks down -Z");

    // Two surfaces at the same view depth, one centred and one far off axis.
    const Vector3 centred(0.0f, 0.0f, -10.0f);
    const Vector3 offAxis(8.0f, 0.0f, -10.0f);
    const float dCentred = forwardSortDistance(centred, camPos, camFwd);
    const float dOffAxis = forwardSortDistance(offAxis, camPos, camFwd);
    check(near(dCentred, 10.0f), "centred surface reads its view depth");
    check(near(dOffAxis, 10.0f), "off-axis surface at the same depth reads the SAME distance");
    check((offAxis - camPos).lengthSquared() > (centred - camPos).lengthSquared(),
        "(the radial form ranked the off-axis one farther — the bug this replaces)");

    // A nearer off-axis surface must sort in front of a farther centred one.
    const Vector3 nearOffAxis(8.0f, 0.0f, -6.0f);
    check(forwardSortDistance(nearOffAxis, camPos, camFwd) < dCentred,
        "nearer off-axis surface sorts in front of a farther centred one");
    check((nearOffAxis - camPos).lengthSquared() == (centred - camPos).lengthSquared(),
        "(radially they were a tie: 8^2 + 6^2 == 10^2)");

    // Behind the camera is negative, not a large positive distance.
    check(forwardSortDistance(Vector3(0.0f, 0.0f, 5.0f), camPos, camFwd) < 0.0f,
        "a point behind the camera reads negative");

    // A rotated, translated camera: the forward vector follows the node.
    const Matrix4 cameraWorld = Matrix4::trs(Vector3(3.0f, 1.0f, -2.0f),
        Quaternion::fromEulerAngles(0.0f, 90.0f, 0.0f), Vector3(1.0f));
    const Vector3 fwd = cameraForwardOf(cameraWorld);
    check(near(fwd.getX(), -1.0f) && near(fwd.getY(), 0.0f) && near(fwd.getZ(), 0.0f, 1e-3f),
        "a camera yawed 90 degrees looks down -X");
    check(near(fwd.length(), 1.0f), "forward vector is unit length");
    check(near(forwardSortDistance(Vector3(-4.0f, 1.0f, -2.0f), Vector3(3.0f, 1.0f, -2.0f), fwd), 7.0f),
        "depth is measured from the camera position along that forward");

    // A scaled camera node must not scale the distance.
    const Matrix4 scaledCamera = Matrix4::trs(Vector3(0.0f), Quaternion(), Vector3(4.0f));
    check(near(cameraForwardOf(scaledCamera).length(), 1.0f), "a scaled camera node still yields a unit forward");

    if (failures == 0) {
        std::cout << "sort distance: all checks passed\n";
        return 0;
    }
    std::cout << "sort distance: " << failures << " check(s) FAILED\n";
    return 1;
}
