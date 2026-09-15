// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// CPU-side geometry of the built-in render primitives (RenderComponent::setType).
//
// TEXTURE ORIGIN: a texture's row 0 is the TOP row of the image, on both
// backends, for loaded images and render targets alike — the same as upstream,
// which stores images unflipped and gives its render targets a top origin. So
// v = 0 has to sit where the top of the picture belongs: at +Y on the box's side
// faces, the sphere and the cone bodies, and at -Z on the plane, which is the top
// edge once the plane is stood up with a +90 degree X rotation. Every primitive
// here writes upstream's `(u, 1 - v)`. The plane did not, and showed every image
// and render texture upside down; tests/primitiveGeometryTests.cpp pins it.
//
#pragma once

#include <cstdint>
#include <vector>

namespace visutwin::canvas
{
    struct PrimitiveGeometry
    {
        std::vector<float> positions;   // xyz per vertex
        std::vector<float> normals;     // xyz per vertex
        std::vector<float> uvs;         // uv per vertex, v = 0 at the image's top row
        // xyzw per vertex, derived from the UVs (scene/geometry/geometryUtils.h):
        // t along +u, bitangent = cross(n, t) * w toward -v, the image's top row.
        std::vector<float> tangents;
        std::vector<uint32_t> indices;
    };

    PrimitiveGeometry createBoxGeometry();
    PrimitiveGeometry createSphereGeometry();
    PrimitiveGeometry createCylinderGeometry();
    PrimitiveGeometry createConeGeometry();
    PrimitiveGeometry createCapsuleGeometry();
    PrimitiveGeometry createPlaneGeometry();
}
