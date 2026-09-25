// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "localShadowFace.h"

#include <memory>
#include <vector>

#include <spdlog/spdlog.h>

#include "depthOnlyDraw.h"
#include "shadowCasterFiltering.h"
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
        // The clear "effect": a fullscreen triangle whose every fragment lands at
        // depth 1.0. No colour output on purpose — the pass has no colour attachment,
        // and a fragment stage that declares one anyway is a MoltenVK hazard that
        // silently drops the depth write (see shadow.frag).
        constexpr const char* kClearDepthMsl = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct ClearDepthVarying {
    float4 position [[position]];
};

vertex ClearDepthVarying clearDepthVertex(ComposeVertexIn in [[stage_in]])
{
    ClearDepthVarying out;
    out.position = float4(in.position.xy, 1.0, 1.0);
    return out;
}

fragment void clearDepthFragment(ClearDepthVarying in [[stage_in]])
{
}
)";

        constexpr const char* kClearDepthGlsl = R"(#version 450

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
void main() { gl_Position = vec4(vertexPosition.xy, 1.0, 1.0); }
#endif

#ifdef VT_FRAGMENT_SHADER
void main() {}
#endif
)";

        std::shared_ptr<Shader> clearDepthShader(GraphicsDevice* device)
        {
            constexpr const char* cacheKey = "clear-depth-quad";
            auto cached = device->getCachedShader(cacheKey);
            if (!cached) {
                ShaderDefinition definition;
                definition.name = cacheKey;
                definition.vshader = "clearDepthVertex";
                definition.fshader = "clearDepthFragment";
                cached = createShader(device, definition,
                    device->shaderLanguage() == ShaderLanguage::Glsl ? kClearDepthGlsl : kClearDepthMsl);
                if (cached) {
                    device->setCachedShader(cacheKey, cached);
                }
            }
            return cached;
        }
    }

    bool bindLocalShadowState(GraphicsDevice* device, ProgramLibrary* programLibrary,
        const Light* light, DepthOnlyShaders& shaders)
    {
        if (!device || !programLibrary || !light) {
            return false;
        }
        auto shadowShader = programLibrary->getShadowShader(nullptr, false);
        auto shadowShaderDynBatch = programLibrary->getShadowShader(nullptr, true);
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
        // positive (acne-removing) polygon offset. Upstream skips the hardware offset
        // for omni lights (they store distance, not depth; this port stores
        // perspective depth and applies a RELATIVE bias in the forward shader) and
        // for PCSS, which biases in the shader.
        const bool skipHardwareBias = light->shadowType() == SHADOW_PCSS_32F ||
            light->type() == LightType::LIGHTTYPE_OMNI;
        const float bias = skipHardwareBias ? 0.0f : light->shadowBias() * -1000.0f;
        device->setDepthBias(bias, bias, 0.0f);
        return true;
    }

    void drawLocalShadowFace(GraphicsDevice* device, ProgramLibrary* programLibrary,
        DepthOnlyShaders& shaders, Light* light, const int face, Camera* shadowCamera)
    {
        if (!device || !programLibrary || !light || !shadowCamera || !shadowCamera->node()) {
            return;
        }

        const Matrix4 viewProjection = shadowCamera->projectionMatrix()
            * shadowCamera->node()->worldTransform().inverse();

        // Build the shadow frustum once for the whole caster sweep.
        const Frustum shadowFrustum = buildCameraFrustum(shadowCamera, shadowCamera->node());

        // Casters. An OMNI light had all six faces classified in one sweep before the
        // frame graph ran (cullShadowCastersOmni), so this face just draws its share:
        // the list is already filtered and needs no frustum test. Every other light
        // collects and culls here — every RenderComponent's mesh instances, plus the
        // batch mesh instances, which belong to no RenderComponent (BatchManager
        // registers them straight with the scene layers) and would otherwise cast
        // no shadow.
        const bool preClassified = light->type() == LightType::LIGHTTYPE_OMNI;
        std::vector<MeshInstance*> casters;
        const std::vector<MeshInstance*>* casterList = &casters;
        if (preClassified) {
            LightRenderData* renderData = light->getRenderData(nullptr, face);
            if (!renderData) {
                return;
            }
            casterList = &renderData->visibleCasters;
        } else {
            collectShadowCasters(casters);
        }

        for (auto* meshInstance : *casterList) {
            if (!meshInstance || !meshInstance->visible()) {
                continue;
            }
            if (!preClassified &&
                !shouldRenderShadowMeshInstance(meshInstance, shadowCamera, shadowFrustum)) {
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
}
