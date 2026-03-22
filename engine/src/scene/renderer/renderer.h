// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <memory>

#include "renderPassUpdateClustered.h"
#include "shadowMap.h"
#include "shadowRenderer.h"
#include "shadowRendererDirectional.h"
#include "shadowRendererLocal.h"
#include "scene/scene.h"
#include "scene/lighting/lightTextureAtlas.h"
#include "scene/lighting/worldClusters.h"

namespace visutwin::canvas
{
    class Camera;
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

    protected:
        std::shared_ptr<GraphicsDevice> _device;

        std::shared_ptr<Scene> _scene;

        std::shared_ptr<RenderPassUpdateClustered> _renderPassUpdateClustered;

        std::unique_ptr<ShadowRendererLocal> _shadowRendererLocal;

        // A list of all unique lights in the layer composition
        std::vector<Light*> _lights;

        // A list of all unique local lights (spot & omni) in the layer composition
        std::vector<Light*> _localLights;

        // A list of unique directional shadow casting lights for each enabled camera.
        // Generated each frame during light culling.
        std::unordered_map<Camera*, std::vector<Light*>> _cameraDirShadowLights;

        ShadowRendererDirectional* shadowRendererDirectional() const { return _shadowRendererDirectional.get(); }

        // ShadowMaps owned by the renderer, kept alive for the lifetime of the lights that use them.
        std::vector<std::unique_ptr<ShadowMap>> _ownedShadowMaps;

    private:
        friend class Engine;
        friend class ShadowRenderer;

        std::unique_ptr<ShadowRenderer> _shadowRenderer;
        std::unique_ptr<ShadowRendererDirectional> _shadowRendererDirectional;

        std::unique_ptr<LightTextureAtlas> _lightTextureAtlas;

        // Clustered lighting: CPU-side 3D grid that indexes local lights.
        // Created lazily when Scene::clusteredLightingEnabled() is true.
        std::unique_ptr<WorldClusters> _worldClusters;

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
        int _numDrawCallsCulled = 0;
        int _camerasRendered = 0;
        int _lightClusters = 0;
        int _gsplatCount = 0;

        std::array<int, PRIMITIVE_TRIFAN - PRIMITIVE_POINTS + 1> _primsPerFrame;
     };
}
