// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// Bakes the light probes of a ProbeVolume in two steps.
//
// captureGeometry captures, for each probe, the geometry it sees: the scene is rendered from the
// probe into the six square faces of its tile of an atlas, every surface writing its albedo,
// normal, distance and emission instead of a colour (Camera::probeVolumeGeometryPass). The
// geometry gives the validity of the probes, their virtual offsets and their visibility, and is
// kept for the lighting.
//
// bakeLighting computes the lighting of the probes from one of two sources:
// - ProbeSource::GBuffer: the captured geometry lit by the engine's lighting without rendering the
//   scene: a quad covering the radiance atlas is drawn with a material that lights each texel as
//   the surface its probe saw through it (Material::probeVolumeLighting). A frame per bounce, so
//   the lighting can be baked again quickly after it changed (the sun moved).
// - ProbeSource::Scene: the scene rendered lit from each probe, a few probes a frame.
// Each bounce lights the scene with the probes of the previous one (Camera::probeVolumeCapture);
// the radiance of the faces is then projected onto the probes' spherical harmonics.
//
// The baker works over several frames: it advances on the engine's update event, and the result
// arrives through the options' onComplete. The geometry stays in memory until the baker is freed,
// for the lighting to be baked again. Baking needs a device that renders to half or full float
// textures (isSupported); probes baked ahead of time load on any device.
//
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/eventHandler.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/constants.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Engine;
    class Entity;
    class GraphicsDevice;
    class Layer;
    class LightComponent;
    class Mesh;
    class MeshInstance;
    class ProbeVolume;
    class RenderTarget;
    class StandardMaterial;
    class Texture;

    /// Where a bake's lighting comes from (ProbeVolumeBakeOptions::source).
    enum class ProbeSource
    {
        /// The captured geometry, lit by the engine's lighting without rendering the scene.
        GBuffer,
        /// The lit scene rendered from each probe: slower, and includes the materials'
        /// view-dependent reflections.
        Scene,
    };

    struct ProbeVolumeBakerOptions
    {
        /// The texels along a side of a face of a probe.
        uint32_t faceSize = 8;
        /// The layers the probes capture.
        std::vector<int> layers = {LAYERID_WORLD, LAYERID_SKYBOX};
        /// Whether the geometry includes the emission of the surfaces.
        bool emissive = false;
        /// The probes captured each frame.
        uint32_t probesPerFrame = 32;
        /// The capture cameras' clip distances.
        float nearClip = 0.05f;
        float farClip = 1000.0f;
    };

    struct ProbeVolumeGeometryResult
    {
        uint32_t probeCount = 0;
        /// The probes inside geometry.
        uint32_t invalidProbes = 0;
        /// The probes captured at an offset position, which made them valid.
        uint32_t movedProbes = 0;
    };

    struct ProbeVolumeCaptureOptions
    {
        /// Find the invalid probes: those seeing more back faces than the threshold, the fraction
        /// of the sphere around the probe.
        bool validity = true;
        float validityThreshold = 0.25f;
        /// Capture the invalid probes at a position moved out of the geometry through the closest
        /// back face, by up to 0.45 of a cell; they are still interpolated at their positions,
        /// while the leak reduction and the visibility use the positions they were captured at.
        bool virtualOffset = true;
        /// Compute the visibility, octahedral maps of visibilitySize texels a side.
        bool visibility = true;
        uint32_t visibilitySize = 16;
        std::function<void(float)> onProgress;
        std::function<void(const ProbeVolumeGeometryResult&)> onComplete;
    };

    struct ProbeVolumeBakeResult
    {
        /// The invalid probes filled from their valid neighbours.
        uint32_t dilatedProbes = 0;
    };

    struct ProbeVolumeBakeOptions
    {
        ProbeSource source = ProbeSource::GBuffer;
        /// The indirect bounces after the direct lighting.
        uint32_t bounces = 1;
        /// Fill the invalid probes from their valid neighbours.
        bool dilation = true;
        /// The shadow bounds of the shadow-casting directional lights while baking: unset, the
        /// volume's box expanded by its blend distance; an empty bounds keeps the shadows fitted
        /// to each camera.
        std::optional<std::optional<BoundingBox>> shadowBounds;
        /// The lighting of the volume to set; lightingCount adds one.
        uint32_t index = 0;
        std::function<void(float)> onProgress;
        std::function<void(const ProbeVolumeBakeResult&)> onComplete;
    };

    class ProbeVolumeBaker
    {
    public:
        ProbeVolumeBaker(Engine* engine, ProbeVolume* volume, const ProbeVolumeBakerOptions& options = {});
        ~ProbeVolumeBaker();

        ProbeVolumeBaker(const ProbeVolumeBaker&) = delete;
        ProbeVolumeBaker& operator=(const ProbeVolumeBaker&) = delete;

        /// Whether a device can bake: it renders to half float or float textures, and draws
        /// something (the null device does not).
        static bool isSupported(const GraphicsDevice* device);

        ProbeVolume* volume() const { return _volume; }
        uint32_t faceSize() const { return _faceSize; }
        const std::vector<int>& layers() const { return _layers; }
        bool emissive() const { return _emissive; }
        /// The probes captured each frame; a change applies from the next capture or bake.
        uint32_t probesPerFrame() const { return _probesPerFrame; }
        void setProbesPerFrame(uint32_t value) { _probesPerFrame = value; }

        /// Whether a capture or a bake is in progress.
        bool baking() const { return _baking; }
        /// Whether the geometry has been captured.
        bool hasGeometry() const { return _geometryTarget != nullptr && !_positions.empty(); }

        /// Starts capturing the geometry; sets the validity, the offsets and the visibility of the
        /// volume. False, starting nothing, on a device that cannot bake or during a bake.
        bool captureGeometry(const ProbeVolumeCaptureOptions& options = {});
        /// Starts baking the lighting, capturing the geometry first if it was not; sets the data
        /// of the volume. False, starting nothing, on a device that cannot bake or during a bake.
        bool bakeLighting(const ProbeVolumeBakeOptions& options = {});

        /// A light fitted to the shadow bounds of a bake, with its own bounds to restore.
        struct ShadowLight
        {
            LightComponent* light = nullptr;
            std::optional<BoundingBox> bounds;
        };
        /// Fits the shadows of the shadow-casting directional lights of the captured layers to the
        /// bounds, which renders each as one shadow map for every camera; empty bounds fit none.
        std::vector<ShadowLight> setShadowBounds(const std::vector<int>& layers,
            const std::optional<BoundingBox>& bounds) const;

        /// Fills the spherical harmonics of the invalid probes from their valid neighbours,
        /// weighted by the inverse square of their distance, four times over, so that probes
        /// surrounded by invalid probes are filled from the probes filled before them. Returns
        /// the probes filled.
        static uint32_t dilate(std::vector<float>& data, const std::vector<uint8_t>& validity,
            const std::array<uint32_t, 3>& resolution);

    private:
        struct CaptureRig
        {
            Entity* entity = nullptr;
            std::vector<CameraComponent*> cameras;
        };
        struct CaptureSetup
        {
            RenderTarget* target = nullptr;
            bool geometry = false;
            std::vector<int> layers;
        };
        struct Atlas
        {
            uint32_t faceSize = 0;
            uint32_t probesPerRow = 0;
            uint32_t width = 0;
            uint32_t height = 0;
        };
        struct Formats
        {
            PixelFormat cubemap = PixelFormat::PIXELFORMAT_RGBA16F;
            PixelFormat target = PixelFormat::PIXELFORMAT_RGBA32F;
        };
        // A step of a capture or a bake, run on an update: Wait to run again on the next update,
        // Done to go on to the next step in the same update.
        enum class StepResult
        {
            Wait,
            Done,
        };
        using Step = std::function<StepResult()>;

        static std::optional<Formats> formatsFor(const GraphicsDevice* device);
        bool begin();
        void onUpdate();
        void finish();

        void queueCapture(const ProbeVolumeCaptureOptions& options);
        void queueBake(const ProbeVolumeBakeOptions& options);

        void createAtlases();
        void destroyGeometry();
        void createRigs();
        Step captureTiles(RenderTarget* target, std::vector<uint32_t> probes, std::vector<float> positions,
            const CaptureSetup& setup, std::function<void(float)> onProgress);
        void clearTiles(RenderTarget* target, const std::vector<std::array<uint32_t, 4>>& rects, bool geometry) const;
        std::array<uint32_t, 4> tileRect(uint32_t probe) const;
        std::vector<float> project(int mode, uint32_t texelsPerProbe, uint32_t probesPerRow, float visibilitySize = 0.0f,
            float maxDistance = 0.0f);
        std::vector<float> captureVisibility(uint32_t size);
        void updatePositionsTexture();

        Step captureSky();
        void createLighting();
        void destroyLighting();

        Engine* _engine = nullptr;
        GraphicsDevice* _device = nullptr;
        ProbeVolume* _volume = nullptr;
        uint32_t _faceSize = 8;
        std::vector<int> _layers;
        bool _emissive = false;
        uint32_t _probesPerFrame = 32;
        float _nearClip = 0.05f;
        float _farClip = 1000.0f;

        bool _baking = false;
        Formats _formats;
        Atlas _atlas;
        std::deque<Step> _steps;
        EventHandlePtr _updateHandle;

        std::vector<std::shared_ptr<Texture>> _geometryBuffers;
        std::shared_ptr<RenderTarget> _geometryTarget;
        std::shared_ptr<Texture> _radianceBuffer;
        std::shared_ptr<RenderTarget> _radianceTarget;
        std::shared_ptr<Texture> _skyAtlas;
        std::shared_ptr<RenderTarget> _skyTarget;
        std::vector<float> _positions;
        std::shared_ptr<Texture> _positionsTexture;
        std::vector<uint8_t> _probeValid;
        std::vector<CaptureRig> _rigs;

        // The lighting of the captured geometry, while a bake from it runs.
        std::shared_ptr<Layer> _lightingLayer;
        std::shared_ptr<Mesh> _lightingMesh;
        std::shared_ptr<StandardMaterial> _lightingMaterial;
        std::unique_ptr<MeshInstance> _lightingMeshInstance;
        Entity* _lightingCamera = nullptr;
        std::vector<std::pair<LightComponent*, std::vector<int>>> _lightingLights;
        std::vector<ShadowLight> _shadowLights;
    };
}
