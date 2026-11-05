// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2026
//
// Rays against the bounding shapes: a ray that starts INSIDE a sphere or a box intersects it,
// and the point reported is where the ray leaves the shape, whether or not a point was asked
// for (the oriented box used to answer true without a point and false with one, and the sphere
// took the absolute value of the entry distance, a point outside it). A shape entirely behind
// the ray still misses, and a ray lying in a min face plane of a box, parallel to it, is inside
// the slab as it is on the max face.

#include <cmath>

#include "core/math/matrix4.h"
#include "core/shape/boundingSphere.h"
#include "core/shape/orientedBox.h"
#include "core/shape/ray.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kEps = 1e-4f;

    bool nearVec(const Vector3& a, const Vector3& b)
    {
        return std::abs(a.getX() - b.getX()) <= kEps && std::abs(a.getY() - b.getY()) <= kEps &&
            std::abs(a.getZ() - b.getZ()) <= kEps;
    }
}

int main()
{
    // ---- sphere ----
    {
        const BoundingSphere sphere(Vector3(0.0f, 0.0f, 0.0f), 2.0f);
        Vector3 point;

        const Ray outside(Vector3(-5.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
        check(sphere.intersectsRay(outside, &point), "sphere: a ray from outside hits");
        check(nearVec(point, Vector3(-2.0f, 0.0f, 0.0f)), "sphere: the entry point of a ray from outside");

        const Ray inside(Vector3(0.5f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
        check(sphere.intersectsRay(inside), "sphere: a ray from inside intersects without a point");
        check(sphere.intersectsRay(inside, &point), "sphere: a ray from inside intersects with a point");
        check(nearVec(point, Vector3(2.0f, 0.0f, 0.0f)), "sphere: the exit point of a ray from inside");

        const Ray behind(Vector3(5.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
        check(!sphere.intersectsRay(behind, &point), "sphere: a sphere behind the ray misses");

        const Ray beside(Vector3(-5.0f, 3.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
        check(!sphere.intersectsRay(beside, &point), "sphere: a ray passing beside misses");
    }

    // ---- oriented box (the axis-aligned slab test, in the box's own space) ----
    {
        const OrientedBox box(Matrix4::identity(), Vector3(1.0f, 2.0f, 3.0f));
        Vector3 point;

        const Ray outside(Vector3(-5.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
        check(box.intersectsRay(outside, &point), "box: a ray from outside hits");
        check(nearVec(point, Vector3(-1.0f, 0.0f, 0.0f)), "box: the entry point of a ray from outside");

        const Ray inside(Vector3(0.0f, 0.5f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        check(box.intersectsRay(inside), "box: a ray from inside intersects without a point");
        check(box.intersectsRay(inside, &point), "box: a ray from inside intersects with a point");
        check(nearVec(point, Vector3(0.0f, 2.0f, 0.0f)), "box: the exit point of a ray from inside");

        const Ray behind(Vector3(0.0f, 0.0f, 5.0f), Vector3(0.0f, 0.0f, 1.0f));
        check(!box.intersectsRay(behind, &point), "box: a box behind the ray misses with a point");
        check(!box.intersectsRay(behind), "box: a box behind the ray misses without a point");

        // in the min face plane x = -1, parallel to it: inside the slab, as on the max face
        const Ray minFace(Vector3(-1.0f, -5.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        check(box.intersectsRay(minFace, &point), "box: a ray in the min face plane intersects");
        check(nearVec(point, Vector3(-1.0f, -2.0f, 0.0f)), "box: its entry point on the min face");
        const Ray maxFace(Vector3(1.0f, -5.0f, 0.0f), Vector3(0.0f, 1.0f, 0.0f));
        check(box.intersectsRay(maxFace, &point), "box: a ray in the max face plane intersects");

        const Ray beside(Vector3(-5.0f, 2.5f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
        check(!box.intersectsRay(beside, &point), "box: a ray passing beside misses");

        // a transformed box: inside, the exit point comes back in world space
        const OrientedBox moved(Matrix4::translation(Vector3(10.0f, 0.0f, 0.0f)), Vector3(1.0f, 1.0f, 1.0f));
        const Ray insideMoved(Vector3(10.0f, 0.0f, 0.0f), Vector3(1.0f, 0.0f, 0.0f));
        check(moved.intersectsRay(insideMoved, &point), "moved box: a ray from inside intersects");
        check(nearVec(point, Vector3(11.0f, 0.0f, 0.0f)), "moved box: the exit point in world space");
    }

    return finish("shape-ray");
}
