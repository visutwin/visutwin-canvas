// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "platform/graphics/graphicsDevice.h"
#include "scene/materials/material.h"
#include "scene/materials/standardMaterial.h"

namespace visutwin::canvas
{
    /**
     * A class responsible for creation and caching of required shaders.
     * There is a two level cache. The first level generates the shader based on the provided options.
     * The second level processes this generated shader using processing options - in most cases
     * modifies it to support uniform buffers.
     */
    class ProgramLibrary
    {
    public:
        ProgramLibrary(const std::shared_ptr<GraphicsDevice>& device, StandardMaterial* standardMaterial);

        void registerProgram(const std::string& name, const std::vector<std::string>& chunkOrder);
        bool hasProgram(const std::string& name) const;

        std::shared_ptr<Shader> getForwardShader(const Material* material, bool transparentPass,
                                                    bool dynamicBatch = false);
        std::shared_ptr<Shader> getShadowShader(bool dynamicBatch = false);

        void bindMaterial(const std::shared_ptr<GraphicsDevice>& device, const Material* material, bool transparentPass,
                          bool dynamicBatch = false);

        // set whether a skybox cubemap is available.
        // When true, skybox materials compile with VT_FEATURE_SKY_CUBEMAP.
        void setSkyCubemapAvailable(bool value) { _skyCubemapAvailable = value; }

        // when true, all forward shaders compile with
        // VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS, overriding fragment output
        // to emit distance-from-reflection-plane. Set per-camera by the renderer.
        void setPlanarReflectionDepthPass(bool value) { _planarReflectionDepthPass = value; }

        // Set when any local light (spot/point) has castShadows enabled.
        // When true, forward shaders compile with VT_FEATURE_LOCAL_SHADOWS.
        void setLocalShadowsEnabled(bool value) { _localShadowsEnabled = value; }

        // Set when any omni light has castShadows enabled.
        // When true, forward shaders compile with VT_FEATURE_OMNI_SHADOWS.
        void setOmniShadowsEnabled(bool value) { _omniShadowsEnabled = value; }

        // Set when clustered lighting is enabled on the scene.
        // When true, forward shaders compile with VT_FEATURE_LIGHT_CLUSTERING.
        void setClusteredLightingEnabled(bool value) { _clusteredLightingEnabled = value; }

        // Set when any area rect light is active in the scene.
        // When true, forward shaders compile with VT_FEATURE_AREA_LIGHTS.
        void setAreaLightsEnabled(bool value) { _areaLightsEnabled = value; }

        // Set when SSAO is in per-material lighting mode (SSAOTYPE_LIGHTING).
        // When true, forward shaders compile with VT_FEATURE_SSAO to sample
        // the SSAO texture during PBR lighting and modulate ambient occlusion.
        void setSsaoEnabled(bool value) { _ssaoEnabled = value; }

        // Set when atmosphere scattering is enabled on the scene.
        // When true, skybox shaders compile with VT_FEATURE_ATMOSPHERE.
        void setAtmosphereEnabled(bool value) { _atmosphereEnabled = value; }

    private:
        struct ShaderVariantOptions
        {
            bool skybox = false;
            bool transparentPass = false;
            bool alphaTest = false;
            bool doubleSided = false;

            bool baseColorMap = false;
            bool normalMap = false;
            bool metallicRoughnessMap = false;
            bool occlusionMap = false;
            bool emissiveMap = false;
            bool envAtlas = true;

            // Feature toggles for future shader chunk ports.
            bool shadowMapping = false;
            bool fog = false;
            bool parallax = false;
            bool clearcoat = false;
            bool anisotropy = false;
            bool sheen = false;
            bool iridescence = false;
            bool transmission = false;
            bool lightClustering = false;
            bool ssao = false;
            bool lightProbes = false;
            bool vertexColors = false;
            bool skinning = false;
            bool morphing = false;
            bool specGloss = false;
            bool orenNayar = false;
            bool detailNormals = false;
            bool displacement = false;
            bool atmosphere = false;
            bool pointSpotAttenuation = false;
            bool multiLight = false;
            bool shadowCatcher = false;
            bool skyCubemap = false;
            bool surfaceLIC = false;    // Surface LIC flow visualization (velocity output + LIC compositing)
            bool instancing = false;    // Hardware instancing — per-instance transform + color via [[instance_id]]
            bool planarReflection = false;  // Planar reflection — screen-space UV sampling + Fresnel blend
            bool planarReflectionDepthPass = false;  // Depth pass: output distance-from-plane instead of PBR
            bool localShadows = false;      // Local light (spot/point) shadow mapping — 2D depth textures
            bool omniShadows = false;       // Omni light cubemap shadow mapping — depthcube textures
            bool dynamicBatch = false;      // Dynamic batching — per-vertex bone index + matrix palette
            bool pointSize = false;         // Point primitive rendering — [[point_size]] in vertex output
            bool areaLights = false;        // Area rectangular lights — MRP evaluation in main loop
            bool unlit = false;             // KHR_materials_unlit — skip PBR lighting
        };

        ShaderVariantOptions buildForwardVariantOptions(const Material* material, bool transparentPass,
                                                         bool dynamicBatch = false) const;
        static std::string resolveProgramName(const ShaderVariantOptions& options);
        uint64_t makeVariantKey(const std::string& programName, const ShaderVariantOptions& options, const Material* material) const;
        std::shared_ptr<Shader> buildForwardShaderVariant(const std::string& programName, const ShaderVariantOptions& options, uint64_t variantKey);

        std::string composeProgramVariantMetalSource(const std::string& programName, const ShaderVariantOptions& options,
            const std::string& vertexEntry, const std::string& fragmentEntry);

        std::shared_ptr<GraphicsDevice> _device;
        std::unordered_map<uint64_t, std::shared_ptr<Shader>> _forwardShaderCache;
        std::unordered_set<std::string> _warnedFeatureFlags;
        std::unordered_map<std::string, std::vector<std::string>> _registeredPrograms;
        bool _skyCubemapAvailable = false;
        bool _planarReflectionDepthPass = false;
        bool _localShadowsEnabled = false;
        bool _omniShadowsEnabled = false;
        bool _clusteredLightingEnabled = false;
        bool _areaLightsEnabled = false;
        bool _ssaoEnabled = false;
        bool _atmosphereEnabled = false;
    };

    // Assigns the program library to the device cache.
    void setProgramLibrary(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<ProgramLibrary>& library);
    std::shared_ptr<ProgramLibrary> getProgramLibrary(const std::shared_ptr<GraphicsDevice>& device);
}
