// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.09.2026
//
// CPU-side geometry of the built-in render primitives (RenderComponent::setType).
//
// TEXTURE ORIGIN: a texture's row 0 is the TOP row of the image, on both
// backends, for loaded images and render targets alike: images are stored
// unflipped and render targets have a top origin. So v = 0 has to sit where the top of the picture belongs: at +Y on the box's side
// faces, the sphere and the cone bodies, and at -Z on the plane, which is the top
// edge once the plane is stood up with a +90 degree X rotation. Every primitive
// here writes `(u, 1 - v)`. The plane did not, and showed every image
// and render texture upside down; tests/primitiveGeometryTests.cpp pins it.
//
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Mesh;

    struct PrimitiveGeometry
    {
        std::vector<float> positions;   // xyz per vertex
        std::vector<float> normals;     // xyz per vertex
        std::vector<float> uvs;         // uv per vertex, v = 0 at the image's top row
        // Lightmap uv per vertex (UV1). The box, cylinder, cone and capsule carry
        // an unwrap with every face or part in its own cell padded by 8/64 of the
        // cell, so a bake gives each its own texels. Empty on the sphere and plane,
        // whose UV1 IS their UV0.
        std::vector<float> uvs1;
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
    /// Torus geometry; the no-argument form is the 'torus' primitive's (tube 0.2,
    /// ring 0.3, 30 segments, 20 sides).
    PrimitiveGeometry createTorusGeometry();
    PrimitiveGeometry createTorusGeometry(float tubeRadius, float ringRadius, float sectorAngleDegrees,
                                          int segments, int sides);
    /// Cone-base geometry: the cylinder, cone and capsule all come from it. A zero
    /// radius at either end is a tip; `roundedCaps` makes hemispheres of radius `peakRadius`.
    PrimitiveGeometry createConeBaseGeometry(float baseRadius, float peakRadius, float height, int heightSegments,
                                             int capSegments, bool roundedCaps);

    /// A mesh in the engine's packed vertex layout, with the
    /// CPU copies a mesh collider or a bake reads. Null for empty geometry.
    std::shared_ptr<Mesh> createMeshFromGeometry(const std::shared_ptr<GraphicsDevice>& device,
                                                 const PrimitiveGeometry& geometry);
}
