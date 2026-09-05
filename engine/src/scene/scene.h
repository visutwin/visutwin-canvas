// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "composition/layerComposition.h"
#include "immediate/immediate.h"
#include "lighting/lightingParams.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/constants.h"
#include "skybox/sky.h"

namespace visutwin::canvas
{
    /**
     * @brief Container for the scene graph, lighting environment, fog, skybox, and layer composition.
     * @ingroup group_scene_renderer
     *
     * A Scene holds all graphical objects, lights, and environment settings that the
     * ForwardRenderer draws each frame. It owns the LayerComposition that controls
     * render order and the LightingParams consumed by the shader system.
     */
    class Scene : public EventHandler
    {
    public:
        static constexpr const char* EVENT_SETLAYERS = "set:layers";

        Scene(const std::shared_ptr<GraphicsDevice>& graphicsDevice);
        ~Scene() = default;

        bool clusteredLightingEnabled() const { return _clusteredLightingEnabled; }
        void setClusteredLightingEnabled(bool value) { _clusteredLightingEnabled = value; }

        const LightingParams& lighting() const { return _lighting; }
        /// Mutable, so an application can set the cluster grid and atlas sizes the
        /// way upstream's `scene.lighting` is written to. Read once per frame by the
        /// renderer, so a change takes effect on the next one.
        LightingParams& lighting() { return _lighting; }
        const std::shared_ptr<LayerComposition>& layers() const { return _layers; }
        const Color& ambientLight() const { return _ambientLight; }
        const FogParams& fog() const { return _fog; }

        void setAmbientLight(float r, float g, float b) { _ambientLight = Color(r, g, b); }

        /** Ambient SH light probes: 9 premultiplied irradiance coefficients
         *  (upstream AMBIENTSH basis). When set, they replace the flat ambient
         *  and the env-atlas irradiance in the lit shader. */
        void setAmbientSH(const std::array<Vector3, 9>& coefficients)
        {
            _ambientSH = coefficients;
            _hasAmbientSH = true;
        }
        void clearAmbientSH() { _hasAmbientSH = false; }
        bool hasAmbientSH() const { return _hasAmbientSH; }
        const std::array<Vector3, 9>& ambientSH() const { return _ambientSH; }

        // Sets the mip level of the skybox to be displayed
        void setSkyboxMip(int value);
        int skyboxMip() const { return _skyboxMip; }

        void setLayers(const std::shared_ptr<LayerComposition>& layers);

        Immediate* immediate() const { return _immediate; }

        void setSkyboxIntensity(float value);
        float skyboxIntensity() const { return _skyboxIntensity; }

        void setExposure(float value) { _exposure = value; }
        float exposure() const { return _exposure; }

        void setSkyType(int value);
        int skyType() const { return _skyType; }

        void setEnvAtlas(Texture* value);
        Texture* envAtlas() const { return _envAtlas; }
        Sky* sky() const { return _sky.get(); }

        // high-res cubemap for skybox rendering.
        // Separate from envAtlas which is only used for PBR lighting.
        void setSkybox(Texture* value);
        Texture* skybox() const { return _skyboxCubeMap; }

        // Local reflection probe: a prefiltered cubemap sampled for specular IBL,
        // optionally box-projected (parallax-corrected) against an axis-aligned
        // volume so reflections align to a room's walls. Overrides the global
        // env-atlas specular for all objects when set. `boxProjection == false`
        // treats the cubemap as an infinite environment (direction only).
        void setReflectionProbe(Texture* cubemap, const Vector3& position,
            const Vector3& boxMin, const Vector3& boxMax, bool boxProjection = true,
            float intensity = 1.0f);
        void clearReflectionProbe() { _reflectionProbeCube = nullptr; }
        Texture* reflectionProbe() const { return _reflectionProbeCube; }
        const Vector3& reflectionProbePosition() const { return _reflectionProbePos; }
        const Vector3& reflectionProbeBoxMin() const { return _reflectionProbeBoxMin; }
        const Vector3& reflectionProbeBoxMax() const { return _reflectionProbeBoxMax; }
        bool reflectionProbeBoxProjection() const { return _reflectionProbeBoxProjection; }
        float reflectionProbeIntensity() const { return _reflectionProbeIntensity; }

        void setToneMapping(int value) { _toneMapping = value; }
        int toneMapping() const { return _toneMapping; }

        bool debugNormalMapsEnabled() const { return _debugNormalMapsEnabled; }
        void setDebugNormalMapsEnabled(const bool enabled) { _debugNormalMapsEnabled = enabled; }
        void setFogEnabled(const bool enabled) { _fog.enabled = enabled; }
        void setFogColor(const Color& color) { _fog.color = color; }
        void setFogLinear(const float start, const float end)
        {
            _fog.start = start;
            _fog.end = end;
        }
        void setFogDensity(const float density) { _fog.density = density; }

        /**
         * Set prefiltered cubemaps and generate an environment atlas from them.
         * Scene.setPrefilteredCubemaps().
         */
        void setPrefilteredCubemaps(const std::vector<Texture*>& cubemaps);

        // Atmosphere scattering (Nishita).
        // Rebuilds the sky mesh: the atmosphere path needs one even with no skybox or env
        // atlas, and Sky::updateSkyMesh only reaches that branch when this flag is already
        // set. Without the rebuild, enabling the atmosphere AFTER setSkyType left the sky
        // permanently reset — the order every caller naturally writes.
        void setAtmosphereEnabled(bool value);
        bool atmosphereEnabled() const { return _atmosphereEnabled; }

        /// Set atmosphere uniforms from bridge data.
        /// data must be a 96-byte AtmosphereUniforms-compatible struct.
        void setAtmosphereUniforms(const void* data, size_t size);

        /// Access raw atmosphere uniform data for the device.
        const void* atmosphereUniformData() const { return &_atmosphereUniforms; }
        size_t atmosphereUniformSize() const { return sizeof(_atmosphereUniforms); }

    private:
        void resetSkyMesh();

        std::shared_ptr<GraphicsDevice> _device;

        // DEVIATION: disabled until clustered lighting (WorldClusters, LightTextureAtlas)
        // is fully ported. With true, the non-clustered local shadow path in
        // ForwardRenderer::buildFrameGraph is skipped and cullLocalLights is never called.
        bool _clusteredLightingEnabled = false;

        LightingParams _lighting;

        // The color of the scene's ambient light, specified in sRGB color space
        Color _ambientLight = Color(0, 0, 0);

        // Premultiplied SH9 irradiance coefficients replacing flat ambient when set
        std::array<Vector3, 9> _ambientSH{};
        bool _hasAmbientSH = false;

        FogParams _fog;

        int _skyboxMip = 0;

        // This flag indicates changes were made to the scene which may require recompilation of
        // shaders that reference global settings
        bool _updateShaders = true;

        std::shared_ptr<LayerComposition> _layers;

        std::unique_ptr<Sky> _sky;

        Immediate* _immediate = nullptr;

        float _skyboxIntensity = 1.0f;
        float _exposure = 1.0f;
        int _skyType = SKYTYPE_INFINITE;

        Texture* _envAtlas = nullptr;
        Texture* _skyboxCubeMap = nullptr;

        Texture* _reflectionProbeCube = nullptr;
        Vector3 _reflectionProbePos{0.0f, 0.0f, 0.0f};
        Vector3 _reflectionProbeBoxMin{-1.0f, -1.0f, -1.0f};
        Vector3 _reflectionProbeBoxMax{1.0f, 1.0f, 1.0f};
        bool _reflectionProbeBoxProjection = true;
        float _reflectionProbeIntensity = 1.0f;

        int _toneMapping = TONEMAP_LINEAR;
        bool _debugNormalMapsEnabled = false;
        bool _atmosphereEnabled = false;

        // Packed atmosphere uniforms (96 bytes). Layout matches GPU AtmosphereData.
        struct alignas(16) AtmosphereUniformsStorage {
            float data[24] = {};
        } _atmosphereUniforms;

        std::vector<Texture*> _prefilteredCubemaps;
        Texture* _internalEnvAtlas = nullptr;
    };
}
