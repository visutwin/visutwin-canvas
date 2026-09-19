// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
//
// CURRENTLY UNREACHABLE. RenderPassCameraFrame::setupDofPass() only does
// `_dofPass.reset()`, so RenderPassDof — and with it this pass and
// RenderPassDofBlur — is never constructed: the multi-pass DOF pipeline
// (CoC -> Downsample -> Blur) is disabled because the parent RenderPassDof has
// no render target, which corrupted the Metal encoder state and produced a black
// screen. Depth of field runs through applyDofSinglePass in the compose shader
// instead, reading the depth buffer directly. Anything "verified" about this
// pass by screenshotting an example is therefore vacuous — it did not run.
// Live since 2026-09-19: RenderPassCameraFrame::setupDofPass builds the pipeline.
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
            float radii[4];  // x=blurRadiusNear, y=blurRadiusFar (both in UV, i.e. / referenceHeight), z=near aspect (h/w, 0 = no near texture), w=far aspect
            float rings[4];  // x=blurRings, y=blurRingPoints
        };
        static_assert(sizeof(DofBlurUniforms) == 32);

        // Upstream's dofBlur chunk over Kernel.concentric, generated in the shader
        // rather than uploaded: a centre tap, then ring r of R at radius r / R with
        // r * P points (upstream's arc spacing works out to exactly that), so the
        // tap count is 1 + P * R * (R + 1) / 2. The step is in UV: the radius is a
        // fraction of a 540-row reference frame, corrected for the texture's aspect,
        // which is what makes the same blurRadius look the same at every resolution.
        // NEAR: an unweighted average of the half-resolution scene. FAR: taps of the
        // CoC-premultiplied far texture, weighted by the CoC at each tap, normalised
        // by the CoC sum and then divided by this pixel's own CoC to undo the
        // premultiply. Texture order far=0, coc=1, near=2 on both backends.
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
    const float2 uv0 = clamp(in.uv, float2(0.0), float2(1.0));
    const float2 coc = cocTexture.sample(linearSampler, uv0, level(0)).rg;
    const float cocFar = coc.r;
    const float cocNear = coc.g;
    const int rings = max(int(u.rings.x), 1);
    const int ringPoints = max(int(u.rings.y), 1);
    float3 sum = float3(0.0);

    // Texture aspects arrive as uniforms: a textureSize() on a combined image sampler
    // does not survive SPIRV-Cross's MSL translation under MoltenVK ("undeclared
    // identifier ..Smplr"), so neither backend queries the texture here.
    if (cocNear > 0.0001 && u.radii.z > 0.0) {
        const float2 step = cocNear * u.radii.x * float2(u.radii.z, 1.0);
        int count = 1;
        sum += nearTexture.sample(linearSampler, uv0, level(0)).rgb;
        for (int ring = 1; ring <= rings; ++ring) {
            const float radius = float(ring) / float(rings);
            const int points = ring * ringPoints;
            for (int p = 0; p < points; ++p) {
                const float angle = float(p) * 6.283185307 / float(points);
                const float2 uv = uv0 + step * float2(cos(angle), sin(angle)) * radius;
                sum += nearTexture.sample(linearSampler, uv, level(0)).rgb;
                ++count;
            }
        }
        sum /= float(count);
    } else if (cocFar > 0.0001) {
        const float2 step = cocFar * u.radii.y * float2(u.radii.w, 1.0);
        float sumCoc = 0.0;
        {
            const float c = cocTexture.sample(linearSampler, uv0, level(0)).r;
            sum += farTexture.sample(linearSampler, uv0, level(0)).rgb * c;
            sumCoc += c;
        }
        for (int ring = 1; ring <= rings; ++ring) {
            const float radius = float(ring) / float(rings);
            const int points = ring * ringPoints;
            for (int p = 0; p < points; ++p) {
                const float angle = float(p) * 6.283185307 / float(points);
                const float2 uv = uv0 + step * float2(cos(angle), sin(angle)) * radius;
                const float c = cocTexture.sample(linearSampler, uv, level(0)).r;
                sum += farTexture.sample(linearSampler, uv, level(0)).rgb * c;
                sumCoc += c;
            }
        }
        if (sumCoc > 0.0) {
            sum /= sumCoc;
        }
        sum /= cocFar;
    }
    return float4(sum, 1.0);
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
    vec2 uv0 = clamp(vUv, vec2(0.0), vec2(1.0));
    vec2 coc = textureLod(cocTexture, uv0, 0.0).rg;
    float cocFar = coc.r;
    float cocNear = coc.g;
    int rings = max(int(u.rings.x), 1);
    int ringPoints = max(int(u.rings.y), 1);
    vec3 sum = vec3(0.0);

    if (cocNear > 0.0001 && u.radii.z > 0.0) {
        vec2 step = cocNear * u.radii.x * vec2(u.radii.z, 1.0);
        int count = 1;
        sum += textureLod(nearTexture, uv0, 0.0).rgb;
        for (int ring = 1; ring <= rings; ++ring) {
            float radius = float(ring) / float(rings);
            int points = ring * ringPoints;
            for (int p = 0; p < points; ++p) {
                float angle = float(p) * 6.283185307 / float(points);
                vec2 uv = uv0 + step * vec2(cos(angle), sin(angle)) * radius;
                sum += textureLod(nearTexture, uv, 0.0).rgb;
                ++count;
            }
        }
        sum /= float(count);
    } else if (cocFar > 0.0001) {
        vec2 step = cocFar * u.radii.y * vec2(u.radii.w, 1.0);
        float sumCoc = 0.0;
        {
            float c = textureLod(cocTexture, uv0, 0.0).r;
            sum += textureLod(farTexture, uv0, 0.0).rgb * c;
            sumCoc += c;
        }
        for (int ring = 1; ring <= rings; ++ring) {
            float radius = float(ring) / float(rings);
            int points = ring * ringPoints;
            for (int p = 0; p < points; ++p) {
                float angle = float(p) * 6.283185307 / float(points);
                vec2 uv = uv0 + step * vec2(cos(angle), sin(angle)) * radius;
                float c = textureLod(cocTexture, uv, 0.0).r;
                sum += textureLod(farTexture, uv, 0.0).rgb * c;
                sumCoc += c;
            }
        }
        if (sumCoc > 0.0) {
            sum /= sumCoc;
        }
        sum /= cocFar;
    }
    fragColor = vec4(sum, 1.0);
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

        // upstream RenderPassDofBlur: the authored radius is a fraction of a 540-row
        // reference frame, so it reads the same at any resolution.
        constexpr float referenceHeight = 540.0f;
        (void)width;
        (void)height;
        DofBlurUniforms uniforms{};
        uniforms.radii[0] = _blurRadiusNear / referenceHeight;
        uniforms.radii[1] = _blurRadiusFar / referenceHeight;
        const auto aspect = [](const Texture* t) {
            return (t && t->width() > 0) ? static_cast<float>(t->height()) / static_cast<float>(t->width()) : 0.0f;
        };
        uniforms.radii[2] = aspect(_nearTexture);
        uniforms.radii[3] = _farTexture ? aspect(_farTexture) : 1.0f;
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

