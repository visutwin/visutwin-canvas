// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025
//
#pragma once

#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/math/blueNoise.h"
#include "core/math/primitives.h"
#include "renderPassUpdateClustered.h"
#include "shadowMap.h"
#include "shadowRenderer.h"
#include "shadowRendererDirectional.h"
#include "shadowRendererLocal.h"
#include "scene/scene.h"
#include "scene/lighting/lightTextureAtlas.h"
#include "scene/graphics/areaLightLuts.h"
#include "scene/lighting/worldClusters.h"

namespace visutwin::canvas
{
    class Camera;
    class LightTextureAtlas;
    class GraphNode;
    class MeshInstance;
    class RenderTarget;
    class Layer;
    class ProgramLibrary;
    class ComponentRegistry;

    /*
     * The base renderer functionality to allow implementation of specialized renderers
     */
    class Renderer
    {
    public:
        Renderer(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<Scene>& scene);

        void renderForwardLayer(Camera* camera, RenderTarget* renderTarget, Layer* layer, bool transparent);

        /// The clustered lighting's shadow and cookie atlases (null outside clustered
        /// lighting), for passes that sample them outside the forward pass (the
        /// volumetric fog's local lights).
        LightTextureAtlas* lightTextureAtlas() const { return _lightTextureAtlas.get(); }

        /// Creates the forward shaders a (camera, layer) sublayer will draw with, ahead
        /// of the frame's first pass (RenderPass::prepareShaders for the forward pass):
        /// every culled draw whose material the library has not resolved yet. Does
        /// nothing on most frames — it is due on the first frame and on the frame after
        /// one that built a variant — because renderForwardLayer makes the same check
        /// itself, exactly, right before it draws; this one exists so that a frame's new
        /// variants start compiling together, before its shadow and effect passes, and
        /// not one layer at a time.
        void prepareForwardShaders(Camera* camera, Layer* layer, bool transparent);

        // Collects directional shadow-casting lights for a camera, allocates shadow maps,
        // and calls ShadowRendererDirectional::cull() to position shadow cameras.
        // Must be called once per frame before buildFrameGraph().
        void cullShadowmaps(Camera* camera);

        /**
         * Clears Light::visibleThisFrame on every light. Call ONCE at the top of a
         * frame, before the per-camera cullLights calls that set it again — the flag
         * is a union over cameras, so a reset inside that loop would leave only the
         * last camera's answer.
         */
        void resetLightVisibility();

        /**
         * Marks the lights this camera's frustum reaches. Must run for every camera before the frame graph
         * is built, because the shadow and cookie passes are built from the result:
         * culling after them would spend a frame's shadow maps on the PREVIOUS
         * frame's answer, which is worse than not culling at all.
         *
         * Directional lights are always marked — their influence has no bounds.
         * Local lights are tested as spheres. One exception: outside
         * clustered lighting a shadow caster with no map yet is marked anyway, so
         * the map gets allocated rather than waiting for the light to be looked at.
         */
        void cullLights(Camera* camera);

        /**
         * Consumes SHADOWUPDATE_THISFRAME on the lights that actually received a
         * shadow pass, and counts the frame's shadow-map updates. Call ONCE, after
         * the frame graph is built.
         *
         * Separate from needsShadowRendering because that predicate is asked more
         * than once per light per frame and, since culling became real, can answer
         * "no" — consuming a one-shot request there threw away the very shadow the
         * caller asked for.
         */
        void consumeOneShotShadows();

        /**
         * The mesh instances of one layer that survived one camera's frustum, split
         * by transparency. Filled once per (camera, layer) per frame, then read by
         * BOTH sublayer passes, so neither sweeps the scene and culls it on its own
         * only to throw away the half that belongs to the other.
         */
        struct CulledInstances
        {
            std::vector<MeshInstance*> opaque;
            std::vector<MeshInstance*> transparent;
            // The frustum this set was culled against. The batch runs while the frame
            // graph is built and the sets are read while it renders, and on the FIRST
            // frame a camera's aspect ratio can still change between the two — the
            // render target is not sized yet — which makes the two frusta disagree.
            // Re-culling on a mismatch keeps the cache a cache rather than a
            // one-frame-stale answer nobody checked.
            Frustum frustum{};
            bool valid = false;
        };

        /**
         * Registers a (camera, layer) pair to be culled this frame, de-duplicated.
         * The frame graph asks for the
         * pairs it will actually render, rather than every combination the layer
         * composition allows.
         */
        void requestMeshInstanceCull(Camera* camera, Layer* layer);

        /**
         * Culls everything requested this frame and clears the requests. Per camera:
         * "precull", then each requested layer, then "postcull";
         * precull comes before the frustum is built so a listener can still move
         * the camera.
         */
        void executeMeshInstanceCull();

        /// Drops last frame's culled sets. Call once at the top of a frame.
        void resetCulledInstances();

        /// Drops the per-frame grid assignments. The pooled grids themselves survive.
        void resetClusters();

        /**
         * The culled set for this pair, culling it now if the frame graph did not ask
         * for it. The fallback is what keeps an unregistered camera — an app-appended
         * pass, say — rendering its layer instead of silently rendering nothing.
         */
        const CulledInstances& culledInstances(Camera* camera, GraphNode* cameraNode, Layer* layer);

        /// Forget the cached cull of (camera, layer), so the next draw of that pair culls
        /// again. For a layer whose CONTENT changed under an unchanged frustum (the
        /// picker's private layer): the cache is keyed on the frustum alone.
        void invalidateCulledInstances(Camera* camera, Layer* layer)
        {
            _culledInstances.erase(std::make_pair(camera, layer));
        }

        // App-injected render passes appended to the END of every frame graph —
        // they run after all scene render actions but before frame end (while the
        // frame's drawable is still valid). Used by extras like OutlineRenderer;
        // rendering to the back buffer AFTER Engine::render() is not safe because
        // frameEnd() presents the drawable.
        void addAppendPass(const std::shared_ptr<RenderPass>& pass)
        {
            _appendPasses.push_back(pass);
        }
        void removeAppendPass(const std::shared_ptr<RenderPass>& pass)
        {
            std::erase(_appendPasses, pass);
        }
        const std::vector<std::shared_ptr<RenderPass>>& appendPasses() const { return _appendPasses; }

        // Per-frame GPU instance culling: for every active MeshInstance that called
        // enableGpuInstanceCulling(), one compute cull per camera in `cameras`, each into
        // that camera's own output (MeshInstance::gpuCullOutputFor), so every view draws
        // what it sees. Runs once a frame, before any pass draws; a draw for a camera not
        // in the list takes the whole instance buffer.
        void dispatchGpuInstanceCulling(const std::vector<Camera*>& cameras);

        /// Sets an ASPECT_AUTO camera's aspect ratio from the viewport it will draw to:
        /// its own render target, or the back buffer, times its rect — the arithmetic
        /// renderForwardLayer uses at draw time.
        void resolveAutoAspectRatio(Camera* camera) const;

    protected:
        /// The components of the engine whose scene this renders (null without a scene).
        ComponentRegistry* componentRegistry() const;

        /// Hands the shadow passes this frame's registry (they collect casters from it).
        void syncShadowComponentRegistry();

        std::vector<std::shared_ptr<RenderPass>> _appendPasses;
        std::shared_ptr<GraphicsDevice> _device;

        std::shared_ptr<Scene> _scene;

        std::shared_ptr<RenderPassUpdateClustered> _renderPassUpdateClustered;

        std::unique_ptr<ShadowRendererLocal> _shadowRendererLocal;

        // Clustered spot-shadow atlas (depth texture2d_array). Accessed by ForwardRenderer.
        std::unique_ptr<LightTextureAtlas> _lightTextureAtlas;

        // A list of all unique lights in the layer composition
        std::vector<Light*> _lights;


        // A list of unique directional shadow casting lights for each enabled camera.
        // Generated each frame during light culling.
        std::unordered_map<Camera*, std::vector<Light*>> _cameraDirShadowLights;


        ShadowRendererDirectional* shadowRendererDirectional() const { return _shadowRendererDirectional.get(); }


    private:
        friend class Engine;
        friend class ShadowRenderer;
        friend struct RendererTestAccess;

        std::unique_ptr<ShadowRenderer> _shadowRenderer;

        // Blue noise (seed 123) and its jitter: advanced once per
        // frame while a camera jitters, and held otherwise.
        BlueNoise _blueNoise{123};
        Vector4 _blueNoiseJitter = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        int _blueNoiseJitterVersion = -1;
        std::unique_ptr<ShadowRendererDirectional> _shadowRendererDirectional;

        // LTC lookup textures for area lights — created lazily on first area light.
        AreaLightLuts::Textures _areaLightLuts;

        /**
         * Clustered lighting grids, one per DISTINCT light set in the frame.
         *
         * A light list IS per layer here, since the gather filters on
         * LightComponent::rendersLayer, so one grid for the frame would give a layer
         * whose lights differ another layer's grid, and leave one with no clustered
         * lights lit by the previous layer's buffers.
         *
         * Grids are keyed on a hash of the layer's light ids and shared between
         * layers that agree. The
         * pool owns them across frames, because a grid holds sizeable cell and light
         * buffers and reallocating per frame would churn; the map is per frame.
         */
        std::vector<std::unique_ptr<WorldClusters>> _clusterPool;
        std::unordered_map<uint64_t, WorldClusters*> _clustersByLightSet;
        size_t _clustersUsedThisFrame = 0;
        ClusterConfig _clusterConfig;
        bool _clusterConfigResolved = false;

        /// The grid for this light set, updated once per frame per distinct set.
        WorldClusters* clustersForLightSet(uint64_t lightSetHash,
            const std::vector<ClusterLightData>& lights);

        /// The key clustersForLightSet shares grids on: independent of the order the
        /// lights were gathered in, so two layers that see the same lights agree.
        static uint64_t lightSetHash(std::vector<const void*> members);

        /// Binds this layer's grid, or ZEROES the grid params when it has no lights —
        /// every layer, since each may be on a different grid.
        void bindLayerClusters(const WorldClusters* clusters);

        /// Builds (or shares) the grid for this layer's clustered lights and binds it
        /// with the clustered shadow atlas.
        void bindLayerClusterLights(const std::vector<ClusterLightData>& lights,
            const std::vector<const void*>& lightSetMembers);

        // renderForwardLayer's stages that need the renderer's own state; the rest
        // are free functions in renderer.cpp.

        /// Sets the frame-wide shader feature switches (lights, sky, SSAO, probes,
        /// atmosphere) for this camera, and binds the area-light LUTs and atmosphere
        /// uniforms they imply.
        bool forwardShaderPreparationDue();
        int _shaderPreparationFrame = -1;
        bool _shaderPreparationDue = false;
        bool _shaderPreparationEver = false;
        uint64_t _shaderPreparationVariantsSeen = 0;

        void configureForwardShaderFeatures(ProgramLibrary& programLibrary, const Camera& camera,
            bool clusteredEnabled);

        void resolveClusterConfig();

        /// The camera the directional cascades were fitted for this frame.
        Camera* directionalShadowFitCamera(Camera* camera) const;

        // Frame statistics are counted into GraphicsDevice::frameCounters(), which the
        // shadow passes and depth-only draws reach too; see frameCounters.h.

        // Per-frame mesh-instance cull cache, keyed by (camera, layer). Emptied by
        // resetCulledInstances at the top of each frame, which keeps an entry (for its
        // storage) only while the pair was culled the frame before; the raw pointer keys
        // are never dereferenced, so one that outlives its camera by a frame is harmless.
        std::map<std::pair<Camera*, Layer*>, CulledInstances> _culledInstances;
        // Cameras registered this frame, in registration order, each with the layers
        // asked for. Consumed and cleared by executeMeshInstanceCull.
        std::vector<Camera*> _cullCameras;
        std::map<Camera*, std::vector<Layer*>> _cullRequests;

        void cullMeshInstancesInto(Camera* camera, GraphNode* cameraNode, Layer* layer,
            CulledInstances& out);
        // Culls one camera's view into a bucket per layer with a single sweep of the scene.
        void cullMeshInstances(Camera* camera, GraphNode* cameraNode, Layer* const* layers,
            CulledInstances* const* outs, size_t layerCount);
     };
}
