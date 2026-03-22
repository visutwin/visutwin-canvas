// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// STL (Stereolithography) file parser for VisuTwin Canvas.
//
// Loads binary and ASCII STL geometry, producing a GlbContainerResource that
// plugs into the existing asset pipeline (same instantiateRenderEntity() path
// as the GLB and OBJ parsers).
//
// STL files have no materials, no UVs, no hierarchy, and only face normals.
// The parser provides optional crease-angle smooth normal generation and
// configurable default PBR material properties.
//
// Custom STL loader (not derived from upstream).
// This is a VisuTwin-specific addition for robot/CAD visualization.
//
#pragma once

#include <memory>
#include <string>

#include "framework/parsers/glbContainerResource.h"

namespace visutwin::canvas
{
    class GraphicsDevice;

    /// Configuration options for STL loading.
    struct StlParserConfig
    {
        /// Uniform scale applied to all vertex positions (e.g., 0.001 for mm -> m).
        float uniformScale = 1.0f;

        /// If true, swap Y and Z axes (Z-up CAD -> Y-up engine) and negate new Z.
        bool flipYZ = false;

        /// If true, reverse face winding order.
        bool flipWinding = false;

        /// Generate smooth normals using crease-angle threshold.
        /// If false, flat shading is used (face normal per triangle).
        bool generateSmoothNormals = false;

        /// Crease angle in degrees for smooth normal generation (only used when
        /// generateSmoothNormals is true). Angles between adjacent face normals
        /// smaller than this threshold are smoothed; larger angles produce hard edges.
        /// 30-45 degrees works well for machined robot parts.
        float creaseAngle = 40.0f;

        /// Generate tangents from normals (Gram-Schmidt fallback since STL has no UVs).
        bool generateTangents = true;

        /// Default material diffuse color (STL has no material data).
        float diffuseR = 0.8f;
        float diffuseG = 0.8f;
        float diffuseB = 0.8f;

        /// Default material metalness (0 = dielectric, 1 = metal).
        float metalness = 0.0f;

        /// Default material roughness (0 = mirror, 1 = matte).
        float roughness = 0.5f;
    };

    class StlParser
    {
    public:
        /// Parse an STL file (binary or ASCII) and return a container resource.
        /// Returns nullptr on failure.
        static std::unique_ptr<GlbContainerResource> parse(
            const std::string& path,
            const std::shared_ptr<GraphicsDevice>& device,
            const StlParserConfig& config = StlParserConfig{});
    };
}
