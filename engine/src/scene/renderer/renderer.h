// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

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
    class GraphNode;
    class MeshInstance;
    class RenderTarget;
    class Layer;

    /*
     * The base renderer functionality to allow implementation of specialized renderers
     */
    class Renderer
    {
    public:
        Renderer(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<Scene>& scene);

        void renderForwardLayer(Camera* camera, RenderTarget* renderTarget, Layer* layer, bool transparent);

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
         * Marks the lights this camera's frustum reaches, as upstream's
         * Culler.cullLights does. Must run for every camera before the frame graph
         * is built, because the shadow and cookie passes are built from the result:
         * culling after them would spend a frame's shadow maps on the PREVIOUS
         * frame's answer, which is worse than not culling at all.
         *
         * Directional lights are always marked — their influence has no bounds.
         * Local lights are tested as spheres. One exception, upstream's: outside
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
         * BOTH sublayer passes — which is the point: the opaque and transparent
         * sublayers used to sweep every RenderComponent in the scene and cull it
         * independently, each throwing away the half that belonged to the other.
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
         * Registers a (camera, layer) pair to be culled this frame, de-duplicated,
         * as upstream's Culler.requestMeshInstanceCull. The frame graph asks for the
         * pairs it will actually render, rather than every combination the layer
         * composition allows.
         */
        void requestMeshInstanceCull(Camera* camera, Layer* layer);

        /**
         * Culls everything requested this frame and clears the requests. Per camera:
         * "precull", then each requested layer, then "postcull" — upstream's order,
         * and precull comes before the frustum is built so a listener can still move
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

        // Per-frame GPU instance culling dispatch: for every visible
        // MeshInstance that has called enableGpuInstanceCulling(), extract
        // frustum planes from `camera` and run the Metal compute cull pass.
        // Overwrites each culler's compacted buffer and indirect args buffer
        // in-place; the renderer's indirect draw path then consumes them. ONE output
        // per culler, filled before any pass draws, so it is called once per frame:
        // with `camera` the instances are culled to its frustum, with null none are
        // culled — what a frame with several cameras needs, since each view would
        // otherwise draw the set culled for whichever camera came last.
        void dispatchGpuInstanceCulling(Camera* camera);

        /// Sets an ASPECT_AUTO camera's aspect ratio from the viewport it will draw to:
        /// its own render target, or the back buffer, times its rect — the arithmetic
        /// renderForwardLayer uses at draw time.
        void resolveAutoAspectRatio(Camera* camera) const;

    protected:
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

        std::unique_ptr<ShadowRenderer> _shadowRenderer;
        std::unique_ptr<ShadowRendererDirectional> _shadowRendererDirectional;

        // LTC lookup textures for area lights — created lazily on first area light.
        AreaLightLuts::Textures _areaLightLuts;

        /**
         * Clustered lighting grids, one per DISTINCT light set in the frame.
         *
         * There used to be exactly one, built by whichever layer rendered first and
         * then bound for every other layer regardless of its own lights — and a light
         * list IS per layer here, since the gather filters on
         * LightComponent::rendersLayer. A layer whose lights differed got another
         * layer's grid; one with no clustered lights at all inherited the previous
         * layer's buffers and kept being lit by them.
         *
         * Upstream's WorldClustersAllocator keys grids on a hash of the layer's light
         * ids and shares one between layers that agree, which is what this does. The
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

        int _forwardDrawCalls = 0;
        int _materialSwitches = 0;
        int _depthMapTime = 0;
        int _forwardTime = 0;
        int _sortTime = 0;

        // timing
        int _skinTime = 0;
        int _morphTime = 0;
        int _cullTime = 0;
        int _shadowMapTime = 0;
        int _lightClustersTime = 0;
        int _layerCompositionUpdateTime = 0;

        int _shadowMapUpdates = 0;
        int _shadowDrawCalls = 0;
        int _skinDrawCalls = 0;
        int _instancedDrawCalls = 0;
        // Per-frame mesh-instance cull cache, keyed by (camera, layer). Cleared by
        // resetCulledInstances at the top of each frame; a camera or layer destroyed
        // mid-frame cannot outlive it, which is why raw pointers are safe as keys.
        std::map<std::pair<Camera*, Layer*>, CulledInstances> _culledInstances;
        // Cameras registered this frame, in registration order, each with the layers
        // asked for. Consumed and cleared by executeMeshInstanceCull.
        std::vector<Camera*> _cullCameras;
        std::map<Camera*, std::vector<Layer*>> _cullRequests;

        void cullMeshInstancesInto(Camera* camera, GraphNode* cameraNode, Layer* layer,
            CulledInstances& out);

        int _numDrawCallsCulled = 0;
        int _camerasRendered = 0;
        int _lightClusters = 0;
        int _gsplatCount = 0;

        std::array<int, PRIMITIVE_TRIFAN - PRIMITIVE_POINTS + 1> _primsPerFrame;
     };
}
