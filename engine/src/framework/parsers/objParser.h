// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// OBJ + MTL file parser for VisuTwin Canvas.
//
// Loads Wavefront OBJ geometry and companion MTL materials using tinyobjloader,
// producing a GlbContainerResource that plugs into the existing asset pipeline
// (same instantiateRenderEntity() path as the GLB parser).
//
// Custom OBJ loader (not derived from upstream).
// This is a VisuTwin-specific addition for robot visualization.
//
#pragma once

#include <memory>
#include <string>

#include "framework/parsers/glbContainerResource.h"

namespace visutwin::canvas
{
    class GraphicsDevice;

    /// Configuration options for OBJ loading.
    struct ObjParserConfig
    {
        /// Uniform scale applied to all vertex positions (e.g., 0.001 for mm -> m).
        float uniformScale = 1.0f;

        /// If true, swap Y and Z axes (Z-up CAD -> Y-up engine) and negate new Z.
        bool flipYZ = false;

        /// If true, reverse face winding order.
        bool flipWinding = false;

        /// Generate smooth normals when the OBJ file has no normals.
        bool generateNormals = true;

        /// Generate tangents for normal mapping.
        bool generateTangents = true;

        /// Override MTL search path (empty = same directory as .obj file).
        std::string mtlSearchPath;
    };

    class ObjParser
    {
    public:
        /// Parse an OBJ file (+ companion MTL) and return a container resource.
        /// Returns nullptr on failure.
        static std::unique_ptr<GlbContainerResource> parse(
            const std::string& path,
            const std::shared_ptr<GraphicsDevice>& device,
            const ObjParserConfig& config = ObjParserConfig{});
    };
}
