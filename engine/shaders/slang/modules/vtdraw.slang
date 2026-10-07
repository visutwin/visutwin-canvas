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
