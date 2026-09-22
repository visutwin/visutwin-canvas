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

        // Columns of the upper 3x3. Component i of the three columns is ROW i.
        Vector3 c0(m.getColumn(0));
        Vector3 c1(m.getColumn(1));
        Vector3 c2(m.getColumn(2));

        // Renormalize axis if scale is to be ignored. The axes renormalized are the ROWS of the
        // 3x3, so row i is scaled by 1 / |row i|, which is component i of every column scaled
        // by the same factor. A zero-length row is left alone.
        if (ignoreScale) {
            const Vector3 rowLengthSq = c0 * c0 + c1 * c1 + c2 * c2;
            const auto invLength = [](const float lengthSq) {
                return lengthSq > 0 ? 1.0f / std::sqrt(lengthSq) : 1.0f;
            };
            const Vector3 scale(invLength(rowLengthSq.getX()), invLength(rowLengthSq.getY()),
                invLength(rowLengthSq.getZ()));
            c0 = c0 * scale;
            c1 = c1 * scale;
            c2 = c2 * scale;
        }

        _center = c0 * ac.getX() + c1 * ac.getY() + c2 * ac.getZ() + m.getTranslation();
        _halfExtents = c0.abs() * ar.getX() + c1.abs() * ar.getY() + c2.abs() * ar.getZ();
    }

    /**
     * Combines two bounding boxes into one, enclosing both.
     * Modifies this bounding box in place.
     */
    void BoundingBox::add(const BoundingBox& other)
    {
        const Vector3 tmin = Vector3::min(_center - _halfExtents, other.center() - other.halfExtents());
        const Vector3 tmax = Vector3::max(_center + _halfExtents, other.center() + other.halfExtents());

        _center = (tmin + tmax) * 0.5f;
        _halfExtents = (tmax - tmin) * 0.5f;
    }
}
