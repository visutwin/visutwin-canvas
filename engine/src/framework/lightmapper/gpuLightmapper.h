// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.08.2026
//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "core/math/quaternion.h"

#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class Engine;
    class Entity;
    class LightComponent;
    class Layer;
    class MeshInstance;
    class RenderTarget;
    class Texture;

    /**
     * GPU lightmap baker: each target
     * mesh is rendered **in UV space**, its unwrap rasterized across its own lightmap
     * render target while the fragment stage evaluates the ordinary lit pipeline at the
     * interpolated world position. Occlusion therefore comes from the engine's existing
     * shadow maps rather than from rays, which is what makes it fast: a bake costs a few
     * frames instead of the CPU baker's per-texel ray casting (see Lightmapper).
     *
     * The bake rides the normal frame graph, the same trick ReflectionProbe uses for its
     * six face cameras: one camera per target mesh, each with `lightmapBakePass` set and
     * its own render target, each rendering a private layer holding only that mesh. Every
     * later frame of the bake adds to the target (additive blending, no clear), which is
     * how the virtual lights accumulate. After the last frame the lightmaps are
     * post-processed (bilateral denoise when
     * `lightmapFilterEnabled`, then dilate, ping-ponged through a temporary target) and
     * become the meshes' lightmaps.
     *
     * The frames, in order:
     *   - with `ambientBake`: the ambient light alone (the scene's ambient or environment
     *     irradiance at each texel); then `ambientBakeNumSamples` frames into a separate
     *     occlusion target, each one of the white virtual directional lights
     *     (BakeLightAmbient) over the top `ambientBakeSpherePart` of the sphere, shadow
     *     mapped; the accumulated visibility is shaped by `ambientBakeOcclusionContrast` /
     *     `Brightness`, saturated and multiplied into the ambient;
     *   - the scene's bake lights (with `ambientBake`, added on top; without it, together
     *     with the unoccluded ambient, which a lightmap REPLACES at runtime here);
     *   - `directionalBakeNumSamples` frames of soft directional shadows (BakeLightSimple).
     *
     * Usage — bake() then update() once per frame after Engine::render():
     *
     *     GpuLightmapper baker(engine);
     *     baker.bake(meshInstances, {.lightmapSizeMultiplier = 512.0f});
     *     // ... engine->update(dt); engine->render(); ...
     *     baker.update();          // advances the bake; true once the lightmaps are applied
     *
     * DEVIATIONS from upstream: no BAKE_COLORDIR (the dominant-direction lightmap would
     * need a second bake output and a directional-lightmap path in both forward chunks);
     * the bake spreads over several rendered frames rather than one synchronous call;
     * lightmaps are always RGBA16F linear, with baked texels
     * marked by alpha (see lightmapFilterShaders.h). The CPU Lightmapper remains available
     * as a ray-traced reference.
     */
    class GpuLightmapper
    {
    public:
        /// These travel with the bake.
        struct Options
        {
            /// Per-mesh resolution from world bounds.
            /// Falls back to `lightmapSize` when zero.
            float lightmapSizeMultiplier = 1.0f;
            int lightmapMaxResolution = 2048;
            int lightmapSize = 256;

            /// Layer id used for the private bake layers. One layer per target is created
            /// starting from this id, so keep the range clear of the app's own layers.
            int baseLayerId = 200;

            /// Soft baked shadows for directional lights: the light is baked as N virtual copies, each
            /// rotated within a `directionalBakeArea`-degree cone and accumulated, which
            /// turns the single hard shadow map into a penumbra. One frame per sample.
            /// They apply to every directional light.
            int directionalBakeNumSamples = 1;
            float directionalBakeArea = 0.0f;

            /// Ambient occlusion via virtual lights. One frame per sample.
            bool ambientBake = false;
            int ambientBakeNumSamples = 1;        // clamped to 1..255
            float ambientBakeSpherePart = 0.4f;   // clamped to 0.001..1
            float ambientBakeOcclusionContrast = 0.0f;
            float ambientBakeOcclusionBrightness = 0.0f;

            /// Bilateral denoise before the dilate.
            bool lightmapFilterEnabled = false;
            float lightmapFilterRange = 10.0f;
            float lightmapFilterSmoothness = 0.2f;

            /// The bake cameras are placed to look at this point from
            /// `bakeCameraDistance` away, which is what the directional shadows get fitted
            /// to. The UV-space vertex stage ignores the camera transform, so this only
            /// steers shadow fitting; leave it at the scene centre and a distance that
            /// covers the whole scene.
            Vector3 bakeCameraTarget{0.0f, 0.0f, 0.0f};
            float bakeCameraDistance = 100.0f;
        };

        explicit GpuLightmapper(Engine* engine);
        ~GpuLightmapper();

        GpuLightmapper(const GpuLightmapper&) = delete;
        GpuLightmapper& operator=(const GpuLightmapper&) = delete;

        /// Set up the bake. Takes effect over the next rendered frames; call update()
        /// after every Engine::render() to advance and collect it.
        void bake(const std::vector<MeshInstance*>& targets, const Options& options);
        void bake(const std::vector<MeshInstance*>& targets) { bake(targets, Options{}); }

        /// Call once per frame AFTER Engine::render(). Sets up the next frame of the bake
        /// or, after the last, post-processes the lightmaps, assigns them to the target
        /// mesh instances, masks the meshes out of realtime lighting and removes the bake
        /// cameras. Returns true on the frame the bake completes.
        bool update();

        /// True while a bake is queued or rendering.
        bool baking() const { return _pending; }

        /// Baked textures, one per target, in the order passed to bake().
        const std::vector<std::shared_ptr<Texture>>& lightmaps() const { return _lightmaps; }

        /// Detaches the baked lightmaps from their mesh instances (or reattaches
        /// them), for A/B toggles.
        void setLightmapsEnabled(bool enabled);

    private:
        /// What the frame about to render bakes.
        enum class Phase
        {
            AmbientLight,          // the ambient alone, into the lightmaps
            AmbientOcclusion,      // one ambient virtual light, into the occlusion targets
            Direct,                // the bake lights (plus the ambient without ambientBake)
            DirectionalSample,     // one soft-shadow copy of the directional lights
            Done
        };

        // bake()'s stages, in the order it runs them.
        /// The lightmap texture, render target, private layer and bake camera of one
        /// target (a null lightmap for a target with no mesh).
        void prepareTarget(size_t index);
        Entity* createBakeCamera(const Layer& layer, const std::shared_ptr<RenderTarget>& renderTarget);
        /// Every scene light reaches the bake layers, and a MASK_BAKE light casts shadows.
        void widenLightsForBake();
        /// Records the directional lights to bake as soft-shadow copies.
        void prepareDirectionalSamples();
        void setupAmbientLight();
        std::vector<int> bakeLayerIds() const;

        /// Configures lights and cameras for `phase` / `sample`, the next frame's work.
        void startPhase(Phase phase, int sample);
        /// Points every bake camera at its lightmap or occlusion target, and sets whether
        /// the frame clears it first and whether it adds to what is there.
        void configureCameras(bool occlusionTargets, bool clear, bool accumulate);
        /// Each scene light back to its authored enabled state, ANDed with `keep`.
        template <typename Predicate>
        void enableSceneLights(Predicate keep);
        void prepareAmbientSample(int index);
        void prepareDirectionalSample(int index);

        // The offline quad passes (lightmapFilterShaders.h).
        /// lightmap = curve(occlusion) x ambient, through a temporary target.
        void applyAmbientOcclusion();
        /// (Denoise or dilate) into the temporary target,
        /// then dilate back into the lightmap.
        void postprocessLightmaps();
        /// The temporary ping-pong target for one lightmap size.
        const std::shared_ptr<RenderTarget>& tempTarget(int size);

        void destroyBakeNodes();

        Engine* _engine = nullptr;
        Options _options;
        bool _pending = false;
        Phase _phase = Phase::Done;
        int _sample = 0;

        std::vector<MeshInstance*> _targets;
        std::vector<std::shared_ptr<Texture>> _lightmaps;
        // Per target, parallel to _cameras (targets without a mesh have none of these).
        std::vector<std::shared_ptr<RenderTarget>> _targetsRT;
        std::vector<std::shared_ptr<Texture>> _occlusionTextures;
        std::vector<std::shared_ptr<RenderTarget>> _occlusionRT;
        std::vector<std::shared_ptr<Layer>> _layers;
        std::vector<Entity*> _cameras;
        std::map<int, std::pair<std::shared_ptr<Texture>, std::shared_ptr<RenderTarget>>> _tempTargets;

        std::vector<uint32_t> _originalMasks;
        std::vector<std::pair<LightComponent*, std::vector<int>>> _lightLayerBackup;
        std::vector<std::pair<LightComponent*, uint32_t>> _lightMaskBackup;
        // Each scene light's own enabled flag, which every phase starts from and the end of
        // the bake restores — a light the app switched off stays off.
        std::vector<std::pair<LightComponent*, bool>> _lightEnabledBackup;

        // The ambient-occlusion virtual light.
        Entity* _ambientLightEntity = nullptr;
        LightComponent* _ambientLight = nullptr;

        // Directional lights baked as virtual copies: their authored rotation, intensity and
        // luminance (whichever the scene's units read), restored before each sample is
        // offset and after the bake.
        std::vector<std::tuple<LightComponent*, Quaternion, float, float>> _directionalLights;
        int _dirSampleCount = 0;
    };
}
