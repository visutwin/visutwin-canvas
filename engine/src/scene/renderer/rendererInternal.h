// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// Internal to the renderer: the types and helpers its translation units (renderer*.cpp)
// share. Not part of the engine's interface; include it from those files only.
#pragma once

#include "renderer.h"

#include "scene/renderer/renderPassOit.h"

#include "framework/components/componentRegistry.h"
#include "cullModeResolve.h"
#include "multiDraw.h"
#include "core/hash.h"
#include "scene/renderer/sortDistance.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <set>

#include "core/scopedTimer.h"
#include "core/objectPool.h"
#include "lightCamera.h"
#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "framework/entity.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/light/lightComponent.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/instanceCuller.h"
#include "scene/frustumUtils.h"
#include "scene/renderer/sortKey.h"
#include "scene/light.h"
#include "scene/morph.h"
#include "scene/morphInstance.h"
#include "scene/skinInstance.h"
#include "scene/gsplat/gsplatInstance.h"
#include "scene/particles/particleEmitter.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/material.h"
#include "scene/materials/materialParameterRead.h"
#include "scene/graphNode.h"
#include "scene/shader-lib/programLibrary.h"
#include "scene/lighting/worldClusters.h"
#include "framework/batching/skinBatchInstance.h"
#include "lightingValidation.h"
#include "renderPassUpdateClustered.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas::renderer_internal
{

        struct LightDispatchEntry
        {
            GpuLightData light;
            uint32_t mask = MASK_AFFECT_DYNAMIC;
            Light* sceneLight = nullptr;  // for clustered spot-shadow atlas lookup
            // Fraction of the viewport height this light's bounds cover, for THIS
            // camera. Ranks local lights when more are visible than the shader has
            // slots. 1 for a directional light, which covers everything.
            float screenSize = 1.0f;
        };


        struct PixelRect
        {
            int x = 0;
            int y = 0;
            int w = 1;
            int h = 1;
        };

        PixelRect normalizedRectToPixels(const Vector4& rect, const int targetWidth, const int targetHeight);

        constexpr int kMaxMainLights = 8;

        bool skyIsDome(const Scene* scene);


        /// What the forward pass knows about the camera it renders for.
        struct ForwardView
        {
            Camera* camera = nullptr;
            GraphNode* cameraNode = nullptr;
            const CameraComponent* cameraComponent = nullptr;
            RenderTarget* activeTarget = nullptr;
            Vector3 position;
            Vector3 forward{0.0f, 0.0f, -1.0f};
            Matrix4 viewMatrix = Matrix4::identity();
            Matrix4 projMatrix = Matrix4::identity();
            Matrix4 viewProjection = Matrix4::identity();
            // The sky's: always a perspective projection, so an orthographic camera still
            // sees the sky around it rather than a flat slab of the sky mesh.
            Matrix4 viewProjectionSkybox = Matrix4::identity();
            PixelRect viewport;
            PixelRect scissor;
            int toneMapping = TONEMAP_LINEAR;
            // Built only for the LIGHT cull — mesh instances are culled in the batch
            // (Renderer::cullMeshInstancesInto) and never touch it.
            bool hasFrustum = false;
            Frustum frustum{};
            // False for a view of a multi-view camera that is a layer over other views
            // (CameraView::background): it draws no sky.
            bool background = true;
        };


        /// This (camera, layer)'s lights, split as the shaders consume them.
        struct ForwardLights
        {
            std::vector<LightDispatchEntry> directional;
            // Sorted by apparent size, largest first.
            std::vector<LightDispatchEntry> local;
            ShadowParams shadowParams{};

            void reset()
            {
                directional.clear();
                local.clear();
                directional.reserve(4);
                local.reserve(8);
                shadowParams = ShadowParams{};
            }
        };

        bool layerMayHaveClusteredLights(const Layer& layer, const Scene* scene);

        void gatherForwardLights(const ForwardView& view, const Layer& layer, const Scene* scene,
            const bool clusteredEnabled, Camera* camera, ForwardLights& out);

        void bindLayerEnvironment(GraphicsDevice& device, const Scene* scene, const ForwardView& view);

        void buildClusterLightInput(const std::vector<LightDispatchEntry>& localLights,
            const bool clusteredShadowsEnabled, const bool clusteredCookiesEnabled, const uint32_t clusteredAreaShapes,
            std::vector<ClusterLightData>& lights, std::vector<const void*>& members);

        void filterLightsForMask(const ForwardLights& lights, const uint32_t mask, const bool clusteredEnabled,
            std::vector<GpuLightData>& out);
}
