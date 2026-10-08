// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 08.10.2026
//
// The local-space vertex hook. Every mesh entry of the forward vertex stage (plain, coloured,
// skinned, morphed, instanced; not the sky, points or dynamic batches, which are not in a
// mesh's own space) passes its vertex through here in the MESH's space, after displacement and
// morphing and before the skin, instance and model transforms. The shadow program shares the
// stage, so a shadow follows whatever this does.
//
// The default changes nothing and compiles away. A chunk override of `forward-vertex-local`
// (Material::setShaderChunk, ShaderChunks::set) replaces this file to deform a mesh, reading
// the material block or any other global of the forward program.
void forwardLocalVertex(inout float3 position, inout float3 normal, inout float4 tangent, float2 uv0, float2 uv1)
{
}
