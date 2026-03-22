// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Assimp-based multi-format 3D model parser for VisuTwin Canvas.
//
// Loads Collada (.dae), FBX (.fbx), 3DS, PLY, and other Assimp-supported
// formats, producing a GlbContainerResource that plugs into the existing
// asset pipeline (same instantiateRenderEntity() path as the GLB parser).
//
// Collada files with Phong/Lambert materials are automatically converted
// to PBR via shininess-to-roughness and specular-to-metalness heuristics.
//
// Custom Assimp loader (not derived from upstream).
// This is a VisuTwin-specific addition for robot/CAD visualization.
//
#pragma once

#include <memory>
#include <string>

#include "framework/parsers/glbContainerResource.h"

namespace visutwin::canvas
{
    class GraphicsDevice;

    /// Configuration options for Assimp-based model loading.
    struct AssimpParserConfig
    {
        /// Uniform scale applied to all vertex positions (e.g., 0.01 for cm -> m).
        float uniformScale = 1.0f;

        /// If true, swap Y and Z axes (Z-up CAD -> Y-up engine) and negate new Z.
        bool flipYZ = false;

        /// If true, reverse face winding order.
        bool flipWinding = false;

        /// Smoothing angle in degrees for aiProcess_GenSmoothNormals (0-180).
        /// 80 degrees works well for organic models; 30-45 for hard-surface/CAD.
        float smoothingAngle = 80.0f;

        /// Generate tangents for normal mapping (via aiProcess_CalcTangentSpace
        /// with Lengyel algorithm fallback).
        bool generateTangents = true;

        /// Merge small meshes sharing the same material to reduce draw calls.
        bool optimizeMeshes = true;
    };

    class AssimpParser
    {
    public:
        /// Parse a 3D model file (DAE, FBX, 3DS, PLY, etc.) and return a container resource.
        /// Returns nullptr on failure.
        static std::unique_ptr<GlbContainerResource> parse(
            const std::string& path,
            const std::shared_ptr<GraphicsDevice>& device,
            const AssimpParserConfig& config = AssimpParserConfig{});
    };
}
