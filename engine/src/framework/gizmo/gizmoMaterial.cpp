// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "gizmoMaterial.h"

#include <map>
#include <string>

#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    namespace
    {
        // The vertex inputs and per-draw buffers the forward pass binds for a custom
        // shader (examples/src/custom-shader-example.cpp has the full list): scene at
        // buffer(1), model at buffer(2), the material block at buffer(3), which the
        // fragment stage reads.
        constexpr const char* kGizmoMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexData {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv0      [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
    float2 uv1      [[attribute(4)]];
};

struct SceneData { float4x4 projViewMatrix; };

struct ModelData {
    float4x4 modelMatrix;
    float4x4 normalMatrix;
    float     normalSign;
    float3    _pad;
};

struct GizmoData {
    float4 color;
    float4 depth;   // x: the depth to write, or negative for the interpolated one
};

struct Varyings {
    float4 position [[position]];
};

struct FragmentOut {
    float4 color [[color(0)]];
    float depth  [[depth(any)]];
};

vertex Varyings gizmoVertex(VertexData v [[stage_in]],
                            constant SceneData &scene [[buffer(1)]],
                            constant ModelData &model [[buffer(2)]])
{
    Varyings out;
    float4 clip = scene.projViewMatrix * model.modelMatrix * float4(v.position, 1.0);
    // Upstream: keep the shape inside the depth range rather than clipping it.
    clip.z = clamp(clip.z, -abs(clip.w), abs(clip.w));
    clip.z = 0.5 * (clip.z + clip.w);   // GL [-1,1] -> [0,1], as every engine vertex shader
    out.position = clip;
    return out;
}

fragment FragmentOut gizmoFragment(Varyings in [[stage_in]],
                                   constant GizmoData &gizmo [[buffer(3)]])
{
    if (gizmo.color.a < 1.0 / 255.0) {
        discard_fragment();
    }
    FragmentOut out;
    out.color = gizmo.color;
    out.depth = gizmo.depth.x >= 0.0 ? gizmo.depth.x : in.position.z;
    return out;
}
)MSL";

        // Vulkan: transforms arrive in the vertex push constant, the material block is
        // set 0 / binding 0 (bound to both stages), and the clip.z remap is mandatory.
        constexpr const char* kGizmoGlsl = R"GLSL(
#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform GizmoData {
    vec4 uColor;
    vec4 uDepth;
} gizmo;

#ifdef VT_VERTEX_SHADER
layout(location = 0) in vec3 vertexPosition;
void main() {
    vec4 clip = pc.viewProjection * pc.model * vec4(vertexPosition, 1.0);
    clip.z = clamp(clip.z, -abs(clip.w), abs(clip.w));
    clip.z = 0.5 * (clip.z + clip.w);
    gl_Position = clip;
}
#endif

#ifdef VT_FRAGMENT_SHADER
layout(location = 0) out vec4 fragColor;
void main() {
    if (gizmo.uColor.a < 1.0 / 255.0) {
        discard;
    }
    fragColor = gizmo.uColor;
    gl_FragDepth = gizmo.uDepth.x >= 0.0 ? gizmo.uDepth.x : gl_FragCoord.z;
}
#endif
)GLSL";

        std::shared_ptr<Shader> sharedGizmoShader(const std::shared_ptr<GraphicsDevice>& device)
        {
            // Held weakly: the materials co-own it, and the last one to go frees it (a
            // strong cache would outlive the device at static destruction).
            static std::map<const GraphicsDevice*, std::weak_ptr<Shader>> cache;
            std::weak_ptr<Shader>& slot = cache[device.get()];
            if (auto shader = slot.lock()) {
                return shader;
            }
            ShaderDefinition definition;
            definition.name = "gizmo-unlit";
            definition.vshader = "gizmoVertex";
            definition.fshader = "gizmoFragment";
            const std::string source = device->shaderLanguage() == ShaderLanguage::Glsl ? kGizmoGlsl : kGizmoMsl;
            auto shader = createShader(device.get(), definition, source);
            slot = shader;
            return shader;
        }
    }

    GizmoMaterial::GizmoMaterial(const std::shared_ptr<GraphicsDevice>& device)
    {
        setName("gizmo-unlit");
        // Upstream: blendType BLEND_NORMAL, depth test and write left at their defaults,
        // back faces culled (the plane shape turns culling off).
        setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        setDepthState(std::make_shared<DepthState>());
        setCullMode(CullMode::CULLFACE_BACK);
        setTransparent(true);
        if (device) {
            setShaderOverride(sharedGizmoShader(device));
        }
    }

    void GizmoMaterial::setColor(const Color& color)
    {
        _color = color;
        _block.color[0] = color.r;
        _block.color[1] = color.g;
        _block.color[2] = color.b;
        _block.color[3] = color.a;
        markUniformsDirty();
    }

    void GizmoMaterial::setDepth(const float depth)
    {
        _depth = depth;
        _block.depth[0] = depth;
        markUniformsDirty();
    }

    const void* GizmoMaterial::customUniformData(size_t& outSize) const
    {
        outSize = sizeof(_block);
        return &_block;
    }
}
