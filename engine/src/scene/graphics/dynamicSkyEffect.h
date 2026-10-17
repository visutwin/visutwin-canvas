// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
// A dynamic sky as a camera frame effect: the effect takes over the sky of its camera with a day
// sky of an analytic atmosphere lit by the sun, a night sky of stars and the moon, and optionally
// clouds drifting with the wind: one flat layer of noise (CloudTechnique::Flat) or volumetric
// clouds ray marched through a layer of 3D noise (Volumetric), which for a perspective camera
// render to a texture of their own at cloudResolution of the scene (their noise is generated on
// the CPU on first use, dynamicSkyNoise.h). The time of day is the sun's azimuth and elevation,
// and the effect keeps a directional light (`light`) in sync with the sun and the moon.
//
// It also lights the scene from its sky: the environment atlas it generates goes to
// Scene::setEnvAtlas (`lighting`), regenerated when the sun or the moon moved by
// lightingThreshold degrees or a setting changed, and with lightingInterpolation blended from
// one update to the next over the time between them. Cameras without the effect render the
// scene's sky from that atlas, a still image of this sky.
//
//     auto sky = std::make_shared<DynamicSkyEffect>();
//     sky->setLight(sunEntity->findComponent<LightComponent>());
//     sky->clouds = true;
//     camera->addFrameEffect(sky);
//
// Its passes: the lighting (preScene, only on frames it is due or blends), the volumetric cloud
// buffer (preScene). The sky itself is a mesh instance on the Skybox layer drawn by the effect's
// camera alone (MeshInstance::setOnlyCamera), while CameraFrameEffect::rendersSky hides the
// scene's sky from that camera.
//
// DEVIATIONS: the clouds drift by the world's time (the effect's dt), as upstream's, but a
// paused world also holds the lighting blend; the lighting is generated in an offline scope from
// a targetless pass (upstream's FramePass), and a blend starts from a shader copy of the atlas
// shown rather than a texture copy, which Vulkan refuses while a frame records.
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "cameraFrameEffect.h"
#include "core/math/color.h"
#include "core/math/vector3.h"
#include "framework/destroyWatch.h"

namespace visutwin::canvas
{
    class DynamicSkyNoise;
    class GraphNode;
    class Layer;
    class LightComponent;
    class Material;
    class Mesh;
    class MeshInstance;
    class RenderTarget;
    class Scene;

    enum class CloudTechnique
    {
        Flat,
        Volumetric,
    };

    class DynamicSkyEffect final : public CameraFrameEffect
    {
    public:
        DynamicSkyEffect();
        ~DynamicSkyEffect() override;

        // ---- the sun and the atmosphere
        /// The sun's compass direction and its elevation above the horizon, in degrees.
        float azimuth = 0.0f;
        float elevation = 25.0f;
        /// Turns the whole sky, the sun and the moon with it, around Y, in degrees.
        float rotation = 0.0f;
        /// Haziness (1-20), the blueness (0-4), the glow around the sun (0-0.1) and how tightly
        /// it hugs the sun (0-1).
        float turbidity = 2.0f;
        float rayleigh = 1.5f;
        float mieCoefficient = 0.005f;
        float mieDirectionalG = 0.8f;
        /// The brightness of the sky, relative to the sun's light it is balanced against.
        float luminance = 1.0f;
        /// The intensity of the sun's light at the zenith, which the sky is balanced against.
        float sunIntensity = 2.0f;

        // ---- the night
        float moonIntensity = 1.0f;
        Color moonColor{0.792f, 0.918f, 1.0f, 1.0f};
        Vector3 moonDirection{-1.53f, 0.85f, 0.35f};
        /// The moon's radius in degrees, and the brightness of its disc.
        float moonSize = 0.6f;
        float moonGlow = 3.0f;
        Color nightColor{0.114f, 0.247f, 0.408f, 1.0f};
        float nightBrightness = 0.052f;
        float starBrightness = 0.05f;
        float starDensity = 0.8f;
        float starSize = 0.5f;
        float twilightGlow = 0.52f;

        // ---- the clouds
        bool clouds = false;
        CloudTechnique cloudTechnique = CloudTechnique::Volumetric;
        /// The volumetric march's steps, 0-1: fewer cheaper, more smoother.
        float cloudQuality = 0.5f;
        /// The resolution the volumetric clouds render at, relative to the scene's, 0.25-1.
        float cloudResolution = 0.5f;
        float cloudCoverage = 0.45f;
        float cloudSoftness = 0.7f;
        float cloudOpacity = 1.0f;
        float cloudBrightness = 0.6f;
        float cloudNightBrightness = 0.05f;
        /// The size of the cloud pattern, its height and the layer's thickness, in metres.
        float cloudSize = 8000.0f;
        float cloudHeight = 2000.0f;
        float cloudThickness = 2500.0f;
        /// How much of the sun's light the clouds take away from the light below them.
        float cloudSunDimming = 0.6f;
        /// The wind, in metres per second, and its direction in degrees.
        float windSpeed = 10.0f;
        float windDirection = 45.0f;

        // ---- the lighting
        /// Lights the scene from the sky (Scene::setEnvAtlas).
        bool lighting = true;
        /// The face size of the cube the lighting is generated from, and the atlas's size.
        int lightingResolution = 256;
        int atlasSize = 512;
        /// The angle in degrees the sun or the moon moves before the lighting is generated again.
        float lightingThreshold = 0.5f;
        /// The drift of the clouds, in pattern repeats, after which it is generated again (0: never).
        float lightingCloudThreshold = 0.0f;
        /// Blends the lighting from one update to the next over the time between them.
        bool lightingInterpolation = false;

        /// The directional light kept in sync with the sun and the moon (its direction, colour and
        /// intensity), or null. A destroyed light's entity clears it.
        void setLight(LightComponent* light);
        [[nodiscard]] LightComponent* light() const { return _light; }

        /// Generates the lighting again before the next frame renders.
        void bakeLighting() { _bakeRequested = true; }

        [[nodiscard]] bool rendersSky() const override { return true; }
        [[nodiscard]] bool ownsPasses() const override { return true; }
        [[nodiscard]] std::string buildKey() const override;
        void createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes) override;
        void destroyPasses() override;
        void update() override;
        void frameUpdate(const CameraFrameEffectContext& context) override;

        /// The world direction toward the sun this frame.
        [[nodiscard]] const Vector3& sunDirection() const { return _sunDir; }
        /// The atlas the scene is lit with (null before the first lighting).
        [[nodiscard]] Texture* envAtlas() const { return _envAtlas.get(); }
        /// How many times the lighting was generated, and the blend toward the latest (1: done).
        [[nodiscard]] int bakeCount() const { return _bakeCount; }
        [[nodiscard]] float blendWeight() const { return _blendWeight; }
        /// Whether the volumetric clouds render to a texture of their own (a perspective camera).
        [[nodiscard]] bool cloudsBuffered() const;
        /// The sky's mesh instance (null without passes).
        [[nodiscard]] MeshInstance* skyMeshInstance() const { return _meshInstance.get(); }
        /// The variant of dynamic-sky.slang the sky draws with.
        [[nodiscard]] const std::string& skyVariant() const { return _skyVariant; }

        /// Called by the lighting pass: generates the lighting on a frame it is due, and blends it.
        void updateLighting();

        /// The block of dynamic-sky.slang (DynamicSkyData), 26 float4.
        struct Block
        {
            float sunDir[4];
            float betaR[4];
            float betaM[4];
            float night[4];
            float nightParams[4];
            float moonDir[4];
            float moonParams[4];
            float cloudParams[4];
            float cloudOffset[4];
            float cloudShading[4];
            float cloudVolume[4];
            float cloudLight[4];
            float cloudLightDir[4];
            float skyRotation[3][4];
            float bufferRay[3][4];
            float bufferView[3][4];
            float bufferProj[4];
            float output[4];
        };
        [[nodiscard]] const Block& block() const { return _block; }

    private:
        class SkyMaterial;
        class CloudPass;
        class LightingPass;

        [[nodiscard]] Vector3 computeSunDir() const;
        [[nodiscard]] Vector3 computeMoonDir() const;
        void updateAtmosphere();
        void updateLight();
        void updateNight();
        void updateClouds();
        void fillSettings(std::array<float, 32>& out) const;
        [[nodiscard]] bool settingsChanged(float tolerance) const;
        void createLightingTargets();
        void destroyLightingTargets();
        void bake();
        void blend();
        void drawQuad(const std::shared_ptr<RenderTarget>& target, const std::string& variant,
            const std::array<Texture*, 4>& inputs);

        LightComponent* _light = nullptr;
        DestroyWatch _lightWatch;

        Block _block{};
        Vector3 _sunDir;
        Vector3 _skyDir;
        Vector3 _moonWorldDir;

        // The sky the effect draws, and the passes.
        std::shared_ptr<GraphicsDevice> _device;
        Scene* _scene = nullptr;
        Layer* _layer = nullptr;
        std::shared_ptr<SkyMaterial> _material;
        std::shared_ptr<Mesh> _mesh;
        std::unique_ptr<GraphNode> _node;
        std::unique_ptr<MeshInstance> _meshInstance;
        std::string _skyVariant;
        std::shared_ptr<LightingPass> _lightingPass;
        std::shared_ptr<CloudPass> _cloudPass;
        std::shared_ptr<Texture> _cloudTexture;
        std::shared_ptr<RenderTarget> _cloudTarget;
        bool _cloudEncoded = false;
        std::shared_ptr<DynamicSkyNoise> _noise;
        int _frameIndex = 0;

        // The lighting.
        std::shared_ptr<Texture> _lightingCube;
        std::array<std::shared_ptr<RenderTarget>, 6> _faceTargets{};
        std::shared_ptr<Texture> _envAtlas;
        std::shared_ptr<RenderTarget> _envAtlasTarget;
        std::shared_ptr<Texture> _atlasLatest;
        std::shared_ptr<Texture> _atlasPrevious;
        std::shared_ptr<RenderTarget> _atlasPreviousTarget;
        float _blendWeight = 1.0f;
        float _blendDuration = 0.0f;
        float _timeSinceBake = 0.0f;
        float _frameDt = 0.0f;
        bool _bakeRequested = true;
        bool _bakeDue = false;
        bool _bakeJump = false;
        bool _bakePending = false;
        int _bakeCount = 0;
        Vector3 _bakedSkyDir{0.0f, -1.0f, 0.0f};
        Vector3 _bakedMoonDir{0.0f, -1.0f, 0.0f};
        std::array<float, 32> _settings{};
        std::array<float, 32> _bakedSettings{};
        float _driftSinceBake = 0.0f;
    };
}
