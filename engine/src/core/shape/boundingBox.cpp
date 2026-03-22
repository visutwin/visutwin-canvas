// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 20.12.2025.
//
#include "boundingBox.h"

namespace visutwin::canvas
{
    void BoundingBox::setFromTransformedAabb(const BoundingBox& aabb, const Matrix4& m, bool ignoreScale)
    {
        const Vector3& ac = aabb.center();
        const Vector3& ar = aabb.halfExtents();
        const float acx = ac.getX();
        const float acy = ac.getY();
        const float acz = ac.getZ();
        const float arx = ar.getX();
        const float ary = ar.getY();
        const float arz = ar.getZ();

        float mx0 = m.getElement(0, 0);
        float mx1 = m.getElement(1, 0);
        float mx2 = m.getElement(2, 0);
        float my0 = m.getElement(0, 1);
        float my1 = m.getElement(1, 1);
        float my2 = m.getElement(2, 1);
        float mz0 = m.getElement(0, 2);
        float mz1 = m.getElement(1, 2);
        float mz2 = m.getElement(2, 2);

        // Renormalize axis if scale is to be ignored
        if (ignoreScale) {
            float lengthSq = mx0 * mx0 + mx1 * mx1 + mx2 * mx2;
            if (lengthSq > 0) {
                const float invLength = 1.0f / std::sqrt(lengthSq);
                mx0 *= invLength;
                mx1 *= invLength;
                mx2 *= invLength;
            }

            lengthSq = my0 * my0 + my1 * my1 + my2 * my2;
            if (lengthSq > 0) {
                const float invLength = 1.0f / std::sqrt(lengthSq);
                my0 *= invLength;
                my1 *= invLength;
                my2 *= invLength;
            }

            lengthSq = mz0 * mz0 + mz1 * mz1 + mz2 * mz2;
            if (lengthSq > 0) {
                const float invLength = 1.0f / std::sqrt(lengthSq);
                mz0 *= invLength;
                mz1 *= invLength;
                mz2 *= invLength;
            }
        }

        _center = Vector3(
            m.getElement(3, 0) + mx0 * acx + mx1 * acy + mx2 * acz,
            m.getElement(3, 1) + my0 * acx + my1 * acy + my2 * acz,
            m.getElement(3, 2) + mz0 * acx + mz1 * acy + mz2 * acz
        );
        _halfExtents = Vector3(
            std::abs(mx0) * arx + std::abs(mx1) * ary + std::abs(mx2) * arz,
            std::abs(my0) * arx + std::abs(my1) * ary + std::abs(my2) * arz,
            std::abs(mz0) * arx + std::abs(mz1) * ary + std::abs(mz2) * arz
        );
    }

    /**
     * Combines two bounding boxes into one, enclosing both.
     * Modifies this bounding box in place.
     */
    void BoundingBox::add(const BoundingBox& other)
    {
        // Cache scalar values for better performance (matching JS pattern)
        const float tcx = _center.getX();
        const float tcy = _center.getY();
        const float tcz = _center.getZ();
        const float thx = _halfExtents.getX();
        const float thy = _halfExtents.getY();
        const float thz = _halfExtents.getZ();
        float tminx = tcx - thx;
        float tmaxx = tcx + thx;
        float tminy = tcy - thy;
        float tmaxy = tcy + thy;
        float tminz = tcz - thz;
        float tmaxz = tcz + thz;

        const float ocx = other.center().getX();
        const float ocy = other.center().getY();
        const float ocz = other.center().getZ();
        const float ohx = other.halfExtents().getX();
        const float ohy = other.halfExtents().getY();
        const float ohz = other.halfExtents().getZ();
        const float ominx = ocx - ohx;
        const float omaxx = ocx + ohx;
        const float ominy = ocy - ohy;
        const float omaxy = ocy + ohy;
        const float ominz = ocz - ohz;
        const float omaxz = ocz + ohz;

        if (ominx < tminx) tminx = ominx;
        if (omaxx > tmaxx) tmaxx = omaxx;
        if (ominy < tminy) tminy = ominy;
        if (omaxy > tmaxy) tmaxy = omaxy;
        if (ominz < tminz) tminz = ominz;
        if (omaxz > tmaxz) tmaxz = omaxz;

        _center = Vector3((tminx + tmaxx) * 0.5f, (tminy + tmaxy) * 0.5f, (tminz + tmaxz) * 0.5f);
        _halfExtents = Vector3((tmaxx - tminx) * 0.5f, (tmaxy - tminy) * 0.5f, (tmaxz - tminz) * 0.5f);
    }
}
