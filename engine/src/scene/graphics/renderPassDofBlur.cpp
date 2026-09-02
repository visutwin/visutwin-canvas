// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassDofBlur.h"

#include <algorithm>
#include <cmath>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        struct alignas(16) DofBlurUniforms
        {
            float radii[4];  // x=blurRadiusNear, y=blurRadiusFar, z=invResX, w=invResY
            float rings[4];  // x=blurRings, y=blurRingPoints
        };
        static_assert(sizeof(DofBlurUniforms) == 32);

        // Disc/bokeh blur: concentric rings weighted by the CoC at each tap.
        // Texture order is far=0, coc=1, near=2 on both backends now — the Vulkan
        // path used to bind near/far/coc in a different order and read the CoC
        // channels swapped (see the divergence note in renderPassCoC.cpp).
        constexpr const char* DOF_BLUR_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct DofBlurVarying {
    float4 position [[position]];
    float2 uv;
};

struct DofBlurUniforms {
    float4 radii;
    float4 rings;
};

vertex DofBlurVarying dofBlurVertex(ComposeVertexIn in [[stage_in]])
{
    DofBlurVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

fragment float4 dofBlurFragment(
    DofBlurVarying in [[stage_in]],
    texture2d<float> farTexture [[texture(0)]],
    texture2d<float> cocTexture [[texture(1)]],
    texture2d<float> nearTexture [[texture(2)]],
    sampler linearSampler [[sampler(0)]],
    constant DofBlurUniforms& u [[buffer(3)]])
{
    const float2 invResolution = u.radii.zw;
    float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
    float2 coc = cocTexture.sample(linearSampler, uv).rg;

    float3 farColor = float3(0.0);
    float farWeight = 0.0;

    float blurRadius = coc.r * u.radii.y;
    int rings = max(int(u.rings.x), 1);
    int ringPoints = max(int(u.rings.y), 1);

    for (int ring = 1; ring <= rings; ring++) {
        float ringRadius = float(ring) / float(rings);
        int pointsInRing = ring * ringPoints;
        for (int p = 0; p < pointsInRing; p++) {
            float angle = float(p) * 6.283185 / float(pointsInRing);
            float2 offset = float2(cos(angle), sin(angle)) * ringRadius * blurRadius;
            float2 sampleUV = uv + offset * invResolution;
            sampleUV = clamp(sampleUV, float2(0.0), float2(1.0));
            float sampleCoc = cocTexture.sample(linearSampler, sampleUV).r;
            float w = sampleCoc;
            farColor += farTexture.sample(linearSampler, sampleUV).rgb * w;
            farWeight += w;
        }
    }

    if (farWeight > 0.0) {
        farColor /= farWeight;
    } else {
        farColor = farTexture.sample(linearSampler, uv).rgb;
    }

    float3 result = farColor;
    if (coc.g > 0.0) {
        float3 nearColor = float3(0.0);
        float nearWeight = 0.0;
        float nearBlurRadius = coc.g * u.radii.x;

        for (int ring = 1; ring <= rings; ring++) {
            float ringRadius = float(ring) / float(rings);
            int pointsInRing = ring * ringPoints;
            for (int p = 0; p < pointsInRing; p++) {
                float angle = float(p) * 6.283185 / float(pointsInRing);
                float2 offset = float2(cos(angle), sin(angle)) * ringRadius * nearBlurRadius;
                float2 sampleUV = uv + offset * invResolution;
                sampleUV = clamp(sampleUV, float2(0.0), float2(1.0));
                float sampleCocNear = cocTexture.sample(linearSampler, sampleUV).g;
                float w = sampleCocNear;
                nearColor += nearTexture.sample(linearSampler, sampleUV).rgb * w;
                nearWeight += w;
            }
        }

        if (nearWeight > 0.0) {
            nearColor /= nearWeight;
            result = mix(result, nearColor, coc.g);
        }
    }

    return float4(result, 1.0);
}
)";

        constexpr const char* DOF_BLUR_GLSL = R"(
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
layout(set = 0, binding = 0) uniform DofBlurUniforms {
    vec4 radii;
    vec4 rings;
} u;
layout(set = 1, binding = 0) uniform sampler2D farTexture;
layout(set = 1, binding = 1) uniform sampler2D cocTexture;
layout(set = 1, binding = 2) uniform sampler2D nearTexture;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

void main() {
    vec2 invResolution = u.radii.zw;
    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));
    vec2 coc = texture(cocTexture, uv).rg;

    vec3 farColor = vec3(0.0);
    float farWeight = 0.0;

    float blurRadius = coc.r * u.radii.y;
    int rings = max(int(u.rings.x), 1);
    int ringPoints = max(int(u.rings.y), 1);

    for (int ring = 1; ring <= rings; ring++) {
        float ringRadius = float(ring) / float(rings);
        int pointsInRing = ring * ringPoints;
        for (int p = 0; p < pointsInRing; p++) {
            float angle = float(p) * 6.283185 / float(pointsInRing);
            vec2 offset = vec2(cos(angle), sin(angle)) * ringRadius * blurRadius;
            vec2 sampleUV = clamp(uv + offset * invResolution, vec2(0.0), vec2(1.0));
            float w = texture(cocTexture, sampleUV).r;
            farColor += texture(farTexture, sampleUV).rgb * w;
            farWeight += w;
        }
    }

    if (farWeight > 0.0) {
        farColor /= farWeight;
    } else {
        farColor = texture(farTexture, uv).rgb;
    }

    vec3 result = farColor;
    if (coc.g > 0.0) {
        vec3 nearColor = vec3(0.0);
        float nearWeight = 0.0;
        float nearBlurRadius = coc.g * u.radii.x;

        for (int ring = 1; ring <= rings; ring++) {
            float ringRadius = float(ring) / float(rings);
            int pointsInRing = ring * ringPoints;
            for (int p = 0; p < pointsInRing; p++) {
                float angle = float(p) * 6.283185 / float(pointsInRing);
                vec2 offset = vec2(cos(angle), sin(angle)) * ringRadius * nearBlurRadius;
                vec2 sampleUV = clamp(uv + offset * invResolution, vec2(0.0), vec2(1.0));
                float w = texture(cocTexture, sampleUV).g;
                nearColor += texture(nearTexture, sampleUV).rgb * w;
                nearWeight += w;
            }
        }

        if (nearWeight > 0.0) {
            nearColor /= nearWeight;
            result = mix(result, nearColor, coc.g);
        }
    }

    fragColor = vec4(result, 1.0);
}
#endif
)";

        // Concentric sample kernel equivalent to Kernel.concentric usage in the upstream engine.
        std::vector<float> makeConcentricKernel(const int rings, const int pointsPerRing)
        {
            std::vector<float> out;
            out.reserve(static_cast<size_t>(rings * pointsPerRing * 2));
            constexpr float twoPi = 6.28318530718f;
            for (int r = 1; r <= rings; ++r) {
                const float radius = static_cast<float>(r) / static_cast<float>(std::max(rings, 1));
                const int points = std::max(pointsPerRing * r, 1);
                for (int i = 0; i < points; ++i) {
                    const float angle = (static_cast<float>(i) / static_cast<float>(points)) * twoPi;
                    out.push_back(std::cos(angle) * radius);
                    out.push_back(std::sin(angle) * radius);
                }
            }
            if (out.empty()) {
                out.push_back(0.0f);
                out.push_back(0.0f);
            }
            return out;
        }
    }

    RenderPassDofBlur::RenderPassDofBlur(const std::shared_ptr<GraphicsDevice>& device, Texture* nearTexture,
        Texture* farTexture, Texture* cocTexture)
        : RenderPassShaderQuad(device), _nearTexture(nearTexture), _farTexture(farTexture), _cocTexture(cocTexture)
    {
        rebuildKernel();
    }

    void RenderPassDofBlur::setBlurRings(const int value)
    {
        const int clamped = std::max(value, 1);
        if (_blurRings != clamped) {
            _blurRings = clamped;
            rebuildKernel();
        }
    }

    void RenderPassDofBlur::setBlurRingPoints(const int value)
    {
        const int clamped = std::max(value, 1);
        if (_blurRingPoints != clamped) {
            _blurRingPoints = clamped;
            rebuildKernel();
        }
    }

    void RenderPassDofBlur::execute()
    {
        if (_kernel.empty()) {
            rebuildKernel();
        }

        const auto gd = device();
        if (!gd) {
            RenderPassShaderQuad::execute();
            return;
        }

        const auto rt = renderTarget();
        if (!rt || !rt->colorBuffer()) {
            RenderPassShaderQuad::execute();
            return;
        }

        const auto width = static_cast<float>(rt->colorBuffer()->width());
        const auto height = static_cast<float>(rt->colorBuffer()->height());
        if (width <= 0.0f || height <= 0.0f) {
            RenderPassShaderQuad::execute();
            return;
        }

        if (!shader()) {
            constexpr const char* cacheKey = "dof-blur-quad";
            auto cached = gd->getCachedShader(cacheKey);
            if (!cached) {
                ShaderDefinition definition;
                definition.name = cacheKey;
                definition.vshader = "dofBlurVertex";
                definition.fshader = "dofBlurFragment";
                cached = createShader(gd.get(), definition,
                    gd->shaderLanguage() == ShaderLanguage::Glsl ? DOF_BLUR_GLSL : DOF_BLUR_MSL);
                if (cached) {
                    gd->setCachedShader(cacheKey, cached);
                }
            }
            setShader(cached);
        }
        if (!shader()) {
            return;
        }

        DofBlurUniforms uniforms{};
        uniforms.radii[0] = _blurRadiusNear;
        uniforms.radii[1] = _blurRadiusFar;
        uniforms.radii[2] = 1.0f / width;
        uniforms.radii[3] = 1.0f / height;
        uniforms.rings[0] = static_cast<float>(_blurRings);
        uniforms.rings[1] = static_cast<float>(_blurRingPoints);

        setQuadTextureBinding(0, _farTexture);
        setQuadTextureBinding(1, _cocTexture);
        setQuadTextureBinding(2, _nearTexture);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
    }

    void RenderPassDofBlur::rebuildKernel()
    {
        _kernel = makeConcentricKernel(_blurRings, _blurRingPoints);
    }
}

