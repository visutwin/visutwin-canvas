// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once

#include <memory>
#include <vector>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Mesh;
    class Texture;
    class StandardMaterial;

    /**
     * Bakes scene lighting into per-mesh lightmap textures sampled at UV1 (the
     * runtime `VT_FEATURE_LIGHTMAP` path adds them to indirect diffuse).
     *
     * DEVIATION: upstream renders each mesh in UV space on the GPU (direct +
     * bounce + AO, HDR, color+dir). This port bakes on the **CPU** — it
     * rasterizes each target mesh's triangles in UV1 space, and per lightmap
     * texel interpolates the world-space surface point + normal, evaluates
     * direct lighting from the registered lights (Lambert × attenuation, with
     * hard shadow rays), adds ambient occlusion via cosine-weighted hemisphere
     * rays, dilates seams, and sRGB-encodes into an RGBA8 texture. Ray tests use
     * brute-force Möller-Trumbore against all registered occluder triangles.
     * Single bounce only (no GI); intended for static scenes and small meshes.
     */
    class Lightmapper
    {
    public:
        struct Light
        {
            LightType type = LightType::LIGHTTYPE_DIRECTIONAL;
            Vector3 direction{0.0f, -1.0f, 0.0f};   // world, points FROM light (directional/spot)
            Vector3 position{0.0f, 0.0f, 0.0f};     // world (point/spot)
            Color color{1.0f, 1.0f, 1.0f, 1.0f};
            float intensity = 1.0f;
            float range = 0.0f;                     // point/spot (0 = infinite for directional)
            float innerConeCos = 0.9f;              // spot
            float outerConeCos = 0.8f;              // spot
            bool castShadows = true;

            // Soft baked shadows (upstream Light.bakeNumSamples / Light.bakeArea).
            // Upstream bakes N *virtual* copies of the light, each rotated within a
            // bakeArea-degree cone; the ray tracer here jitters the shadow ray over
            // the same cone and averages visibility, which is equivalent and cheaper.
            // DIRECTIONAL ONLY, matching upstream (BakeLightSimple::numVirtualLights).
            int bakeNumSamples = 1;
            float bakeArea = 0.0f;                  // degrees of spread, 0 = hard shadows
        };

        struct Options
        {
            int lightmapSize = 256;
            Color ambient{0.0f, 0.0f, 0.0f, 1.0f};  // flat ambient added everywhere
            bool ambientOcclusion = true;
            int aoSamples = 16;                     // hemisphere rays per texel
            float aoRadius = 4.0f;                  // AO ray max distance (world units)
            Color skyColor{0.0f, 0.0f, 0.0f, 1.0f}; // color added by unoccluded AO rays
            int dilatePixels = 4;                   // seam-fill iterations

            // Automatic per-mesh resolution from the target's world-space bounds
            // (upstream Scene.lightmapSizeMultiplier / lightmapMaxResolution). When
            // sizeMultiplier > 0 it replaces lightmapSize, using upstream's formula:
            // nextPow2(sqrt(sum of the three face areas) * multiplier), capped.
            float sizeMultiplier = 0.0f;
            int maxResolution = 1024;

            // Ambient bake (upstream Scene.ambientBake*). Instead of scaling a flat
            // sky colour by an AO fraction, this distributes ambientBakeNumSamples
            // rays over the top ambientBakeSpherePart of the sphere (upstream's
            // Fibonacci spherePointDeterministic distribution), weights them by N·L,
            // and shapes the resulting occlusion with upstream's bakeLmEnd curve:
            //   ao = saturate(((ao - 0.5) * max(contrast + 1, 0)) + 0.5 + brightness)
            bool ambientBake = false;
            int ambientBakeNumSamples = 20;
            float ambientBakeSpherePart = 0.4f;
            float ambientBakeOcclusionContrast = 0.0f;
            float ambientBakeOcclusionBrightness = 0.0f;

            // Bilateral denoise over the baked texels (upstream Scene.lightmapFilter*).
            // range is the spatial sigma in texels, smoothness the intensity sigma.
            bool filterEnabled = false;
            float filterRange = 10.0f;
            float filterSmoothness = 0.2f;
        };

        explicit Lightmapper(GraphicsDevice* device);

        void addLight(const Light& light) { _lights.push_back(light); }
        /// Register triangles that block shadow/AO rays (world-space). Include
        /// every mesh that should cast into the bake — targets included.
        void addOccluder(const Mesh& mesh, const Matrix4& worldTransform);
        void clear();

        /// Bake a lightmap for one target mesh (world transform applied) and
        /// return the uploaded RGBA8 texture (nullptr on failure).
        std::shared_ptr<Texture> bake(const Mesh& target, const Matrix4& worldTransform,
            const Options& options);

        /// Bake and assign the result to a material's lightmap slot.
        void bakeAndApply(StandardMaterial* material, const Mesh& target,
            const Matrix4& worldTransform, const Options& options);

    private:
        struct Tri { Vector3 a, b, c; };

        GraphicsDevice* _device;
        std::vector<Light> _lights;
        std::vector<Tri> _occluders;   // world-space triangles for ray casting
    };
}
