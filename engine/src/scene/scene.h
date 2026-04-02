// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

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
        const std::shared_ptr<LayerComposition>& layers() const { return _layers; }
        const Color& ambientLight() const { return _ambientLight; }
        const FogParams& fog() const { return _fog; }

        void setAmbientLight(float r, float g, float b) { _ambientLight = Color(r, g, b); }

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
        void setAtmosphereEnabled(bool value) { _atmosphereEnabled = value; }
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
