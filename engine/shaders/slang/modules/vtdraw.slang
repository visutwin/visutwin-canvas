// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The per-draw transforms the forward pass hands a mesh draw, for a program that draws
// meshes with its own shader (a material's shader override). The two backends deliver them
// differently and the difference lives here: Metal binds the scene block (view-projection)
// at buffer 1 and the model block at buffer 2 for the vertex stage, Vulkan pushes the
// view-projection and the model matrix as one 128-byte push-constant block. Each target
// prunes the declaration it does not read.
module vtdraw;

struct SceneData
{
    float4x4 projViewMatrix;
};

struct ModelData
{
    float4x4 modelMatrix;
    float4x4 normalMatrix;
};

struct DrawPushConstants
{
    float4x4 viewProjection;
    float4x4 model;
};

// Metal only: read inside a Metal __target_switch case, so the SPIR-V never references
// them and the Vulkan bindings named here (to keep Slang from warning) are never laid out.
[[vk::binding(61, 1)]] ConstantBuffer<SceneData> vtScene : register(b1);
[[vk::binding(62, 1)]] ConstantBuffer<ModelData> vtModel : register(b2);
[[vk::push_constant]] ConstantBuffer<DrawPushConstants> vtPush;

public float4x4 drawViewProjection()
{
    __target_switch
    {
    case metal:
        return vtScene.projViewMatrix;
    default:
        return vtPush.viewProjection;
    }
}

public float4x4 drawModel()
{
    __target_switch
    {
    case metal:
        return vtModel.modelMatrix;
    default:
        return vtPush.model;
    }
}

/// The model's normal matrix (the inverse transpose of its 3x3, signed so a mirrored model's
/// normals still face out). Metal reads the one the CPU uploads with the model block
/// (Matrix4::normalMatrix); Vulkan has no room for it in the push constants and builds the
/// same cofactor matrix per vertex. The two are equal by construction, to rounding.
public float3x3 drawNormalMatrix()
{
    __target_switch
    {
    case metal:
        return float3x3(vtModel.normalMatrix[0].xyz, vtModel.normalMatrix[1].xyz, vtModel.normalMatrix[2].xyz);
    default:
        return cofactorNormalMatrix(vtPush.model);
    }
}

/// The signed cofactor matrix of `model`'s 3x3: its columns are cross products of the other
/// two model columns, divided out by the determinant's SIGN only (the shader normalises, so
/// the magnitude cancels). A singular 3x3 (a zero scale on some axis) has no inverse
/// transpose, and gets the identity, as Matrix4::normalMatrix gives one.
public float3x3 cofactorNormalMatrix(float4x4 model)
{
    const float3 c0 = model[0].xyz;
    const float3 c1 = model[1].xyz;
    const float3 c2 = model[2].xyz;
    const float3 cof0 = cross(c1, c2);
    const float det = dot(c0, cof0);
    if (det == 0.0) {
        return float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
    }
    const float detSign = det < 0.0 ? -1.0 : 1.0;
    return detSign * float3x3(cof0, cross(c2, c0), cross(c0, c1));
}
