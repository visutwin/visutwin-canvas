// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 16.09.2026
//
#include "localShadowFace.h"

#include <memory>
#include <vector>

#include <spdlog/spdlog.h>

#include "depthOnlyDraw.h"
#include "shadowCasterFiltering.h"
#include "scene/shader-lib/slangShaders.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "scene/camera.h"
#include "scene/frustumUtils.h"
#include "scene/graphNode.h"
#include "scene/graphics/quadRender.h"
#include "scene/light.h"
#include "scene/meshInstance.h"
#include "scene/shader-lib/programLibrary.h"

namespace visutwin::canvas
{
    namespace
    {
        // The clear "effect": engine/shaders/slang/programs/clear-depth.slang, a
        // fullscreen triangle whose every fragment lands at depth 1.0, with no colour output.
        std::shared_ptr<Shader> clearDepthShader(GraphicsDevice* device)
        {
            return getOrCreateSlangShader(device, "clear-depth");
        }
    }

    void prepareLocalShadowShaders(const std::shared_ptr<GraphicsDevice>& device, const bool clearsRects,
        const bool vsm)
    {
        if (!device) {
            return;
        }
        prepareDepthOnlyShaders(device, vsm);
        if (clearsRects) {
            (void)clearDepthShader(device.get());
        }
    }

    bool bindLocalShadowState(GraphicsDevice* device, ProgramLibrary* programLibrary,
        const Light* light, DepthOnlyShaders& shaders, const bool vsm)
    {
        if (!device || !programLibrary || !light) {
            return false;
        }
        auto shadowShader = programLibrary->getShadowShader(nullptr, false, false, false, false, false, vsm);
        auto shadowShaderDynBatch = programLibrary->getShadowShader(nullptr, true, false, false, false, false, vsm);
        if (!shadowShader) {
            // Returning here draws NOTHING into the shadow map, which then reads as
            // its cleared 1.0 and lights every fragment: a total, silent loss of
            // shadows that looks like a shading bug rather than a missing shader.
            // A program-registration mismatch did exactly this on Vulkan, so say it
            // out loud once instead of failing quietly.
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("No shadow shader for this device — local-light "
                    "shadows are disabled");
            }
            return false;
        }
        // The remaining variants are fetched lazily on first use by drawDepthOnly.
        shaders.plain = shadowShader;
        shaders.dynamicBatch = shadowShaderDynBatch;
        shaders.vsm = vsm;

        // This pass bypasses materials. Clear any binding left by the previous
        // forward pass (commonly the skybox at the end of the preceding frame)
        // before the backend resolves its vertex-stage and pipeline state.
        device->setMaterial(nullptr);
        device->setShader(shadowShader);

        // Shadow pass needs blend/depth state set on the device — the forward pass
        // sets these per-material, but the shadow pass bypasses materials entirely.
        // Matches renderPassShadowDirectional.cpp execute().
        static auto shadowBlendState = std::make_shared<BlendState>();   // default: no blend, color writes on
        static auto shadowDepthState = std::make_shared<DepthState>();   // default: depth test+write enabled
        device->setBlendState(shadowBlendState);
        device->setDepthState(shadowDepthState);

        // Hardware polygon-offset depth bias during shadow rendering. See
        // renderPassShadowDirectional: the internal bias is negative, so this is a
        // positive (acne-removing) polygon offset. The hardware offset is skipped
        // for omni lights (this port stores perspective depth and applies a
        // RELATIVE bias in the forward shader), for PCSS, which biases in the
        // shader, and for VSM, whose moments carry no depth-buffer bias.
        const bool skipHardwareBias = light->shadowType() == SHADOW_PCSS_32F || vsm ||
            light->type() == LightType::LIGHTTYPE_OMNI;
        const float bias = skipHardwareBias ? 0.0f : light->shadowBias() * -1000.0f;
        device->setDepthBias(bias, bias, 0.0f);
        return true;
    }

    void drawLocalShadowFace(GraphicsDevice* device, ProgramLibrary* programLibrary,
        DepthOnlyShaders& shaders, Light* light, const int face, Camera* shadowCamera,
        ComponentRegistry* registry)
    {
        if (!device || !programLibrary || !light || !shadowCamera || !shadowCamera->node()) {
            return;
        }

        const Matrix4 viewProjection = shadowCamera->projectionMatrix()
            * shadowCamera->node()->worldTransform().inverse();

        // The face's casters, prepared this frame when the light was culled
        // (ShadowRendererLocal::cullLocalLights): already through the caster rules and
        // the face's frustum, so the pass draws them and nothing else. The list is this
        // frame's or it is not used — it holds raw pointers.
        LightRenderData* renderData = light->getRenderData(nullptr, face);
        if (renderData && renderData->visibleCastersFrame == device->renderVersion()) {
            for (auto* meshInstance : renderData->visibleCasters) {
                if (drawDepthOnly(device, programLibrary, meshInstance, viewProjection, shaders)) {
                    device->frameCounters().shadowDrawCalls++;
                }
            }
            return;
        }

        // No list for this frame: collect and cull here, with the same collector — every
        // RenderComponent's mesh instances, plus the batch mesh instances, which belong
        // to no RenderComponent and would otherwise cast no shadow.
        const Frustum shadowFrustum = buildCameraFrustum(shadowCamera, shadowCamera->node());
        std::vector<MeshInstance*> casters;
        collectShadowCasters(casters, registry);
        for (auto* meshInstance : casters) {
            if (!meshInstance || !meshInstance->visible()) {
                continue;
            }
            if (!shouldRenderShadowMeshInstance(meshInstance, shadowCamera, shadowFrustum)) {
                continue;
            }
            if (drawDepthOnly(device, programLibrary, meshInstance, viewProjection, shaders)) {
                device->frameCounters().shadowDrawCalls++;
            }
        }
    }

    void clearDepthRect(GraphicsDevice* device, const Vector4& rect)
    {
        if (!device) {
            return;
        }
        const auto shader = clearDepthShader(device);
        if (!shader) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                spdlog::warn("No depth-clear shader for this device — atlas shadow "
                    "slots are not cleared between renders");
            }
            return;
        }
        // Depth test ALWAYS + write: every covered texel takes the triangle's 1.0
        // whatever it held. Cull off, because the quad's winding is not the shadow
        // pass's concern.
        static auto clearDepthState = [] {
            auto state = std::make_shared<DepthState>();
            state->setFunc(CompareFunction::Always);
            return state;
        }();
        static auto clearBlendState = std::make_shared<BlendState>();
        device->setMaterial(nullptr);
        device->setBlendState(clearBlendState);
        device->setDepthState(clearDepthState);
        device->setDepthBias(0.0f, 0.0f, 0.0f);
        const CullMode previousCull = device->cullMode();
        device->setCullMode(CullMode::CULLFACE_NONE);
        QuadRender quad(shader);
        quad.render(&rect, &rect);
        device->setCullMode(previousCull);
    }

    void prepareClearDepthShader(GraphicsDevice* device)
    {
        if (device) {
            (void)clearDepthShader(device);
        }
    }

    void clearDepthInPass(GraphicsDevice* device)
    {
        if (!device) {
            return;
        }
        const auto shader = clearDepthShader(device);
        if (!shader) {
            return;
        }
        static auto clearDepthState = [] {
            auto state = std::make_shared<DepthState>();
            state->setFunc(CompareFunction::Always);
            return state;
        }();
        // the colour attachments keep what the earlier layers drew
        static auto noColorWrites = [] {
            auto state = std::make_shared<BlendState>();
            state->setRedWrite(false);
            state->setGreenWrite(false);
            state->setBlueWrite(false);
            state->setAlphaWrite(false);
            return state;
        }();
        device->setMaterial(nullptr);
        device->setBlendState(noColorWrites);
        device->setDepthState(clearDepthState);
        device->setDepthBias(0.0f, 0.0f, 0.0f);
        const CullMode previousCull = device->cullMode();
        device->setCullMode(CullMode::CULLFACE_NONE);
        QuadRender quad(shader);
        quad.render();
        device->setCullMode(previousCull);
    }
}
