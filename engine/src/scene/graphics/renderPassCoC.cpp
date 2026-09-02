// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Circle-of-confusion pass for depth of field: writes (cocFar, cocNear) from
// linear scene depth. One implementation over QuadRender — a shader, one input
// texture and one uniform block — rather than a pass class per backend.
//
// DIVERGENCE worth fixing separately: upstream's coc.js puts a dead zone of
// +/- focusRange/2 around the focus distance (farRange = focus + range * 0.5)
// and ramps over the full focusRange. This port ramps straight from the focus
// distance, which is what the Metal path has always done; the Vulkan path used
// to do something third (dead zone, but ramped over the HALF range, and with
// the two channels swapped). Both backends now agree on Metal's behaviour and
// on upstream's (cocFar, cocNear) channel order, so aligning the ramp with
// upstream is a one-line change in a single place.
//
#include "renderPassCoC.h"

#include <algorithm>

#include "scene/camera.h"
#include "scene/graphics/quadRender.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        struct alignas(16) CoCUniforms
        {
            float focus[4];  // x=focusDistance, y=focusRange, z=cameraNear, w=cameraFar
            float flags[4];  // x=nearBlur
        };
        static_assert(sizeof(CoCUniforms) == 32);

        constexpr const char* COC_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct CoCVarying {
    float4 position [[position]];
    float2 uv;
};

struct CoCUniforms {
    float4 focus;
    float4 flags;
};

vertex CoCVarying cocVertex(ComposeVertexIn in [[stage_in]])
{
    CoCVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

fragment float4 cocFragment(
    CoCVarying in [[stage_in]],
    depth2d<float> depthTexture [[texture(0)]],
    sampler linearSampler [[sampler(0)]],
    constant CoCUniforms& u [[buffer(3)]])
{
    float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    float rawDepth = depthTexture.sample(linearSampler, uv);
    float linearDepth = getLinearDepth(rawDepth, u.focus.z, u.focus.w);

    float cocFar = saturate((linearDepth - u.focus.x) / u.focus.y);

    if (u.flags.x > 0.5) {
        float cocNear = saturate((u.focus.x - linearDepth) / u.focus.y);
        return float4(cocFar, cocNear, 0.0, 1.0);
    }
    return float4(cocFar, 0.0, 0.0, 1.0);
}
)";

        constexpr const char* COC_GLSL = R"(
#version 450

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
layout(location = 2) in vec2 vertexUv0;
layout(location = 0) out vec2 vUv;
void main() {
    vUv = vertexUv0;
    gl_Position = vec4(vertexPosition, 1.0);
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(set = 0, binding = 0) uniform CoCUniforms {
    vec4 focus;
    vec4 flags;
} u;
layout(set = 1, binding = 0) uniform sampler2D depthTexture;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

float getLinearDepth(float rawDepth, float cameraNear, float cameraFar) {
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

void main() {
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));
    float rawDepth = texture(depthTexture, uv).r;
    float linearDepth = getLinearDepth(rawDepth, u.focus.z, u.focus.w);

    float cocFar = clamp((linearDepth - u.focus.x) / u.focus.y, 0.0, 1.0);

    float cocNear = 0.0;
    if (u.flags.x > 0.5) {
        cocNear = clamp((u.focus.x - linearDepth) / u.focus.y, 0.0, 1.0);
    }
    fragColor = vec4(cocFar, cocNear, 0.0, 1.0);
}
#endif
)";
    }

    RenderPassCoC::RenderPassCoC(const std::shared_ptr<GraphicsDevice>& device, CameraComponent* cameraComponent,
        const bool nearBlur)
        : RenderPassShaderQuad(device), _cameraComponent(cameraComponent), _nearBlur(nearBlur)
    {
    }

    void RenderPassCoC::execute()
    {
        _params[0] = _focusDistance + 0.001f;
        _params[1] = std::max(_focusRange, 0.001f);
        _params[2] = 1.0f / _params[1];

        const auto* camera = _cameraComponent ? _cameraComponent->camera() : nullptr;
        if (!camera) return;

        const auto gd = device();
        if (!gd) return;

        Texture* depthTexture = gd->sceneDepthMap();
        if (!depthTexture) {
            return;
        }

        if (!shader()) {
            constexpr const char* cacheKey = "dof-coc-quad";
            auto cached = gd->getCachedShader(cacheKey);
            if (!cached) {
                ShaderDefinition definition;
                definition.name = cacheKey;
                definition.vshader = "cocVertex";
                definition.fshader = "cocFragment";
                cached = createShader(gd.get(), definition,
                    gd->shaderLanguage() == ShaderLanguage::Glsl ? COC_GLSL : COC_MSL);
                if (cached) {
                    gd->setCachedShader(cacheKey, cached);
                }
            }
            setShader(cached);
        }
        if (!shader()) {
            return;
        }

        CoCUniforms uniforms{};
        uniforms.focus[0] = _focusDistance;
        uniforms.focus[1] = std::max(_focusRange, 0.001f);
        uniforms.focus[2] = camera->nearClip();
        uniforms.focus[3] = camera->farClip();
        uniforms.flags[0] = _nearBlur ? 1.0f : 0.0f;

        setQuadTextureBinding(0, depthTexture);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
    }
}
