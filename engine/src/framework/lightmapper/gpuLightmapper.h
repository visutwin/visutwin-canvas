// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <cstdint>
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
    class Entity;
    class Layer;
    class MeshInstance;
    class RenderTarget;
    class StandardMaterial;
    class Texture;

    /**
     * GPU lightmap baker — upstream's approach (`framework/lightmapper`): each target
     * mesh is rendered **in UV space**, its unwrap rasterized across its own lightmap
     * render target while the fragment stage evaluates the ordinary lit pipeline at the
     * interpolated world position. Occlusion therefore comes from the engine's existing
     * shadow maps rather than from rays, which is what makes it fast: a bake costs one
     * frame instead of the CPU baker's per-texel ray casting (see Lightmapper).
     *
     * The bake rides the normal frame graph, the same trick ReflectionProbe uses for its
     * six face cameras: one camera per target mesh, each with `lightmapBakePass` set and
     * its own render target, each rendering a private layer holding only that mesh. After
     * the frame, the targets become the meshes' lightmaps and the bake cameras switch off.
     *
     * Usage — bake() then update() once per frame after Engine::render():
     *
     *     GpuLightmapper baker(engine);
     *     baker.bake(meshInstances, {.sizeMultiplier = 512.0f});
     *     // ... engine->update(dt); engine->render(); ...
     *     baker.update();          // assigns lightmaps once the frame has rendered
     *
     * DEVIATIONS from upstream: no ambient-occlusion virtual lights (upstream bakes N
     * virtual directional lights for the ambient term — here ambient comes from the
     * scene's own ambient/env atlas as evaluated by the lit shader), no bounce passes,
     * no BAKE_COLORDIR, and no GPU dilate/denoise — seams rely on the conservative UV
     * expansion the rasterizer already gives plus the material's bilinear filtering.
     * The CPU Lightmapper remains available and stays the higher-quality reference:
     * it ray-traces true AO and soft shadows at the cost of seconds per mesh.
     */
    class GpuLightmapper
    {
    public:
        struct Options
        {
            /// Per-mesh resolution from world bounds (upstream lightmapSizeMultiplier).
            /// Falls back to `lightmapSize` when zero.
            float sizeMultiplier = 512.0f;
            int maxResolution = 1024;
            int lightmapSize = 256;

            /// Layer id used for the private bake layers. One layer per target is created
            /// starting from this id, so keep the range clear of the app's own layers.
            int baseLayerId = 200;

            /// Soft baked shadows for directional lights (upstream Light.bakeNumSamples /
            /// bakeArea, via BakeLightSimple): the light is baked as N virtual copies, each
            /// rotated within a `directionalBakeArea`-degree cone and accumulated, which
            /// turns the single hard shadow map into a penumbra. One frame per sample.
            int directionalBakeNumSamples = 1;
            float directionalBakeArea = 0.0f;

            /// Ambient occlusion via virtual lights (upstream BakeLightAmbient): instead of
            /// one flat ambient term, the sky is sampled as `ambientBakeNumSamples` virtual
            /// directional lights spread over the top `ambientBakeSpherePart` of the sphere,
            /// each shadow-mapped, accumulated additively into the lightmap. Costs one extra
            /// rendered frame per sample, so it trades bake time for contact darkening.
            bool ambientBake = false;
            int ambientBakeNumSamples = 16;
            float ambientBakeSpherePart = 0.4f;

            /// The bake cameras are placed to look at this point from `bakeCameraDistance`
            /// away, which is what the directional shadow cascades get fitted to. The UV-space
            /// vertex stage ignores the camera transform, so this only steers shadow fitting;
            /// leave it at the scene centre and a distance that covers the whole scene.
            Vector3 bakeCameraTarget{0.0f, 0.0f, 0.0f};
            float bakeCameraDistance = 100.0f;
        };

        explicit GpuLightmapper(Engine* engine);
        ~GpuLightmapper();

        GpuLightmapper(const GpuLightmapper&) = delete;
        GpuLightmapper& operator=(const GpuLightmapper&) = delete;

        /// Set up the bake. Takes effect over the next rendered frame; call update()
        /// after Engine::render() to collect it.
        void bake(const std::vector<MeshInstance*>& targets, const Options& options);
        void bake(const std::vector<MeshInstance*>& targets) { bake(targets, Options{}); }

        /// Call once per frame AFTER Engine::render(). Assigns the baked textures to the
        /// target materials, masks the meshes out of realtime lighting and disables the
        /// bake cameras. Returns true on the frame the bake completes.
        bool update();

        /// True while a bake is queued or rendering.
        bool baking() const { return _pending; }

        /// Baked textures, one per target, in the order passed to bake().
        const std::vector<std::shared_ptr<Texture>>& lightmaps() const { return _lightmaps; }

        /// Restores the materials' pre-bake state (no lightmap), for A/B toggles.
        void setLightmapsEnabled(bool enabled);

    private:
        void destroyBakeNodes();

        Engine* _engine = nullptr;
        Options _options;
        bool _pending = false;
        int _framesWaited = 0;

        std::vector<MeshInstance*> _targets;
        std::vector<std::shared_ptr<Texture>> _lightmaps;
        std::vector<std::shared_ptr<RenderTarget>> _targetsRT;
        std::vector<std::shared_ptr<Layer>> _layers;
        std::vector<Entity*> _cameras;
        std::vector<StandardMaterial*> _materials;
        std::vector<uint32_t> _originalMasks;
        std::vector<std::pair<LightComponent*, std::vector<int>>> _lightLayerBackup;
        std::vector<std::pair<LightComponent*, uint32_t>> _lightMaskBackup;

        // Ambient-occlusion virtual light state. Sample -1 is the direct-light pass;
        // samples 0..N-1 each aim the virtual light at one point on the sphere part.
        void setupAmbientLight();
        void prepareAmbientSample(int index);
        void prepareDirectionalSample(int index);
        void beginAccumulation();
        Entity* _ambientLightEntity = nullptr;
        LightComponent* _ambientLight = nullptr;
        int _ambientSample = -1;
        float _ambientNormalization = 0.0f;
        int _dirSample = -1;
        int _dirSampleCount = 0;
        bool _accumulating = false;
        bool _ambientSuppressed = false;
        // Directional lights baked as virtual copies: their authored rotation and intensity,
        // restored before each sample is offset and after the bake.
        std::vector<std::tuple<LightComponent*, Quaternion, float>> _directionalLights;
        Color _savedAmbient{0.0f, 0.0f, 0.0f, 1.0f};
        Texture* _savedEnvAtlas = nullptr;
    };
}
