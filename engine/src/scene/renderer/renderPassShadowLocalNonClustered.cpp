// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 06.12.2025.
//
#include "renderPassShadowLocalNonClustered.h"

#include <vector>

#include <spdlog/spdlog.h>

#include "core/scopedTimer.h"
#include "framework/components/render/renderComponent.h"
#include "framework/batching/batchManager.h"
#include "framework/batching/skinBatchInstance.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include <scene/graphNode.h>
#include "scene/morph.h"
#include "scene/shader-lib/programLibrary.h"
#include "depthOnlyDraw.h"
#include "localShadowFace.h"
#include "shadowCasterFiltering.h"
#include "scene/frustumUtils.h"

namespace visutwin::canvas
{
    RenderPassShadowLocalNonClustered::RenderPassShadowLocalNonClustered(const std::shared_ptr<GraphicsDevice>& device,
        ShadowRenderer* shadowRenderer, Light* light, int face, bool applyVsm): RenderPass(device),
          _shadowRenderer(shadowRenderer),
          _light(light),
          _graphicsDevice(device),
          _face(face),
          _applyVsm(applyVsm) {

        _requiresCubemaps = false;

        // Prepare the shadow camera for this face
        _shadowCamera = shadowRenderer->prepareFace(light, nullptr, face);

        // Set up the render pass
        // Clear the render target as well, as it contains a single shadow map
        shadowRenderer->setupRenderPass(this, _shadowCamera, true);

        // Set debug name
        if (light->node()) {
            _name = _name + "-" + light->node()->name();
        }
    }

    void RenderPassShadowLocalNonClustered::execute()
    {
        if (!_graphicsDevice || !_shadowCamera || !_shadowCamera->node()) {
            return;
        }
        const ScopedMilliseconds shadowTimer(_graphicsDevice->frameCounters().shadowMapTime);
        auto programLibrary = getProgramLibrary(_graphicsDevice);
        if (!programLibrary) {
            return;
        }
        // This target is the light's own map, cleared by the pass's load action;
        // the face draws into all of it.
        DepthOnlyShaders shaders;
        if (!bindLocalShadowState(_graphicsDevice.get(), programLibrary.get(), _light, shaders)) {
            return;
        }
        drawLocalShadowFace(_graphicsDevice.get(), programLibrary.get(), shaders,
            _light, _face, _shadowCamera);
        (void)_shadowRenderer;
        (void)_applyVsm;
    }
}
