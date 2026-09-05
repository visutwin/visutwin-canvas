// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassDepthAwareBlur.h"

#include "framework/components/camera/cameraComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "scene/camera.h"

namespace visutwin::canvas
{
    namespace
    {
        struct alignas(16) BlurUniforms
        {
            float invResAndDir[4];  // xy = 1/resolution, zw = blur direction
            float params[4];        // x = filterSize, y = cameraNear, z = cameraFar
        };
        static_assert(sizeof(BlurUniforms) == 32);

        // Bilateral blur that respects depth discontinuities, so AO does not halo
        // across silhouettes. Direction is a uniform rather than two compiled
        // variants (the Metal pass carried a HORIZONTAL/VERTICAL source pair).
        constexpr const char* BLUR_MSL = R"(
#include <metal_stdlib>
using namespace metal;

struct ComposeVertexIn {
    float3 position [[attribute(0)]];
    float3 normal [[attribute(1)]];
    float2 uv0 [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float2 uv1 [[attribute(4)]];
};

struct BlurVarying {
    float4 position [[position]];
    float2 uv;
};

struct BlurUniforms {
    float4 invResAndDir;
    float4 params;
};

vertex BlurVarying blurVertex(ComposeVertexIn in [[stage_in]])
{
    BlurVarying out;
    out.position = float4(in.position, 1.0);
    out.uv = in.uv0;
    return out;
}

static inline float getLinearDepth(float rawDepth, float cameraNear, float cameraFar)
{
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

// Point-sampled depth. These passes reconstruct view-space positions from depth,
// and a bilinear tap straddling a silhouette returns a depth that belongs to
// NEITHER surface — a position in mid-air that the kernel then treats as real.
// The Vulkan side binds a nearest sampler for the same reason.
constexpr sampler blurDepthSampler(coord::normalized, filter::nearest,
                                   mip_filter::none, address::clamp_to_edge);

static inline float bilateralWeight(float depth, float sampleDepth)
{
    float diff = (sampleDepth - depth);
    return max(0.0, 1.0 - diff * diff);
}

fragment float4 blurFragment(
    BlurVarying in [[stage_in]],
    texture2d<float> sourceTexture [[texture(0)]],
    depth2d<float> depthTexture [[texture(1)]],
    sampler linearSampler [[sampler(0)]],
    constant BlurUniforms& u [[buffer(3)]])
{
    const float2 sourceInvResolution = u.invResAndDir.xy;
    const float2 direction = u.invResAndDir.zw;
    const int filterSize = int(u.params.x);
    const float cameraNear = u.params.y;
    const float cameraFar = u.params.z;

    const float2 uv = clamp(in.uv, float2(0.0), float2(1.0));

    // handle the center pixel separately because it doesn't participate in bilateral filtering
    float depth = getLinearDepth(depthTexture.sample(blurDepthSampler, uv), cameraNear, cameraFar);
    float totalWeight = 1.0;
    float color = sourceTexture.sample(linearSampler, uv).r;
    float sum = color * totalWeight;

    // Gaussian sigma: filterSize / 3 gives ~99.7% of the bell within the kernel
    float sigma = max(float(filterSize) / 3.0, 1.0);
    float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    for (int i = -filterSize; i <= filterSize; i++) {
        // The center pixel is pre-seeded above; tapping it again would double-count it.
        if (i == 0) { continue; }
        float weight = exp(-float(i * i) * invSigma2);
        float2 offset = direction * float(i) * sourceInvResolution;
        float2 position = uv + offset;

        float tapColor = sourceTexture.sample(linearSampler, position).r;
        float textureDepth = getLinearDepth(depthTexture.sample(blurDepthSampler, position), cameraNear, cameraFar);
        float bilateral = bilateralWeight(depth, textureDepth) * weight;
        sum += tapColor * bilateral;
        totalWeight += bilateral;
    }

    float ao = sum / totalWeight;
    return float4(ao, 0.0, 0.0, 1.0);
}
)";

        constexpr const char* BLUR_GLSL = R"(
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
layout(set = 0, binding = 0) uniform BlurUniforms {
    vec4 invResAndDir;
    vec4 params;
} u;
layout(set = 1, binding = 0) uniform sampler2D sourceTexture;
layout(set = 1, binding = 1) uniform sampler2D depthTexture;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

float getLinearDepth(float rawDepth, float cameraNear, float cameraFar) {
    return (cameraNear * cameraFar) / (cameraFar - rawDepth * (cameraFar - cameraNear));
}

float bilateralWeight(float depth, float sampleDepth) {
    float diff = (sampleDepth - depth);
    return max(0.0, 1.0 - diff * diff);
}

void main() {
    vec2 sourceInvResolution = u.invResAndDir.xy;
    vec2 direction = u.invResAndDir.zw;
    int filterSize = int(u.params.x);
    float cameraNear = u.params.y;
    float cameraFar = u.params.z;

    vec2 uv = clamp(vUv, vec2(0.0), vec2(1.0));

    float depth = getLinearDepth(texture(depthTexture, uv).r, cameraNear, cameraFar);
    float totalWeight = 1.0;
    float color = texture(sourceTexture, uv).r;
    float sum = color * totalWeight;

    float sigma = max(float(filterSize) / 3.0, 1.0);
    float invSigma2 = 1.0 / (2.0 * sigma * sigma);

    for (int i = -filterSize; i <= filterSize; i++) {
        if (i == 0) { continue; }
        float weight = exp(-float(i * i) * invSigma2);
        vec2 position = uv + direction * float(i) * sourceInvResolution;

        float tapColor = texture(sourceTexture, position).r;
        float textureDepth = getLinearDepth(texture(depthTexture, position).r, cameraNear, cameraFar);
        float bilateral = bilateralWeight(depth, textureDepth) * weight;
        sum += tapColor * bilateral;
        totalWeight += bilateral;
    }

    fragColor = vec4(sum / totalWeight, 0.0, 0.0, 1.0);
}
#endif
)";
    }

    RenderPassDepthAwareBlur::RenderPassDepthAwareBlur(const std::shared_ptr<GraphicsDevice>& device,
        Texture* sourceTexture, CameraComponent* cameraComponent, const bool horizontal)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture),
          _cameraComponent(cameraComponent), _horizontal(horizontal)
    {
    }

    void RenderPassDepthAwareBlur::execute()
    {
        const auto gd = device();
        if (!gd || !_sourceTexture || !_cameraComponent || !_cameraComponent->camera()) {
            return;
        }

        Texture* depthTexture = gd->sceneDepthMap();
        if (!depthTexture) {
            return;
        }

        const auto rt = renderTarget();
        if (!rt || !rt->colorBuffer()) {
            return;
        }

        const auto* camera = _cameraComponent->camera();

        if (!shader()) {
            constexpr const char* cacheKey = "depth-aware-blur-quad";
            auto cached = gd->getCachedShader(cacheKey);
            if (!cached) {
                ShaderDefinition definition;
                definition.name = cacheKey;
                definition.vshader = "blurVertex";
                definition.fshader = "blurFragment";
                cached = createShader(gd.get(), definition,
                    gd->shaderLanguage() == ShaderLanguage::Glsl ? BLUR_GLSL : BLUR_MSL);
                if (cached) {
                    gd->setCachedShader(cacheKey, cached);
                }
            }
            setShader(cached);
        }
        if (!shader()) {
            return;
        }

        BlurUniforms uniforms{};
        uniforms.invResAndDir[0] = 1.0f / static_cast<float>(rt->colorBuffer()->width());
        uniforms.invResAndDir[1] = 1.0f / static_cast<float>(rt->colorBuffer()->height());
        uniforms.invResAndDir[2] = _horizontal ? 1.0f : 0.0f;
        uniforms.invResAndDir[3] = _horizontal ? 0.0f : 1.0f;
        uniforms.params[0] = 8.0f;  // filterSize
        uniforms.params[1] = camera->nearClip();
        uniforms.params[2] = camera->farClip();

        setQuadTextureBinding(0, _sourceTexture);
        setQuadTextureBinding(1, depthTexture);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
    }
}
