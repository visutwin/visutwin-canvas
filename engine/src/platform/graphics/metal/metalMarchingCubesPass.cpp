// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Metal compute pass for GPU Marching Cubes -- implementation.
//
// Contains the embedded MSL compute kernels (classifyCells + generateVertices)
// and CPU-side dispatch logic for the two-pass atomic pipeline.
//
// Custom shader -- no upstream GLSL equivalent exists.
//
#include "metalMarchingCubesPass.h"

#include "metalGraphicsDevice.h"
#include "metalTexture.h"
#include "spdlog/spdlog.h"

#include <cstring>

namespace visutwin::canvas
{
    namespace
    {
        // ── Embedded Metal Shading Language ─────────────────────────────
        //
        // Two compute kernels for GPU Marching Cubes isosurface extraction.
        //
        // Pass 1 (classifyCells): Classify each cell and count total vertices.
        // Pass 2 (generateVertices): Generate vertex data with atomic allocation.
        //
        // The output vertex format matches the engine's standard VertexData
        // layout (56 bytes): position(3) + normal(3) + uv0(2) + tangent(4) + uv1(2).
        //
        // Lookup tables (edgeTable, triTable) match the CPU implementation
        // in viz/algorithms/marchingCubes.h (Lorensen & Cline convention).
        //
        constexpr const char* MC_COMPUTE_SOURCE = R"(
#include <metal_stdlib>
using namespace metal;

// ── Uniform parameters ──────────────────────────────────────────────
// Must match MCComputeParams in metalMarchingCubesPass.h (80 bytes).
struct MCParams {
    uint  dimsX, dimsY, dimsZ;
    float isovalue;
    float domainMinX, domainMinY, domainMinZ;
    float _pad0;
    float domainMaxX, domainMaxY, domainMaxZ;
    float _pad1;
    float texelSizeX, texelSizeY, texelSizeZ;
    uint  maxVertices;
    uint  flipNormals;
    float _pad2[3];
};

// ── Output vertex layout ────────────────────────────────────────────
// 56 bytes, matches the engine's common.metal VertexData:
//   attribute(0) float3  position    offset  0
//   attribute(1) float3  normal      offset 12
//   attribute(2) float2  uv0         offset 24
//   attribute(3) float4  tangent     offset 32
//   attribute(4) float2  uv1         offset 48
struct VertexData {
    packed_float3 position;     // 12 bytes
    packed_float3 normal;       // 12 bytes
    packed_float2 uv0;          //  8 bytes
    packed_float4 tangent;      // 16 bytes
    packed_float2 uv1;          //  8 bytes
};

// ── Helpers ─────────────────────────────────────────────────────────

// Sample volume at exact voxel coordinate (integer grid position).
// Uses texel-center addressing: uvw = (gridPos + 0.5) / dims.
inline float sampleVoxel(texture3d<float> volume, sampler s,
                         int ix, int iy, int iz,
                         float3 invDims)
{
    float3 uvw = (float3(ix, iy, iz) + 0.5f) * invDims;
    return volume.sample(s, uvw).x;
}

// Convert grid coordinates to world position.
inline float3 gridToWorld(int gx, int gy, int gz,
                          float3 domainMin, float3 domainMax,
                          float3 texelSize)
{
    return domainMin + float3(gx, gy, gz) * texelSize * (domainMax - domainMin);
}

// Interpolate vertex position along an edge between two corners.
inline float3 interpolateEdge(float3 p0, float3 p1,
                              float v0, float v1, float iso)
{
    float denom = v1 - v0;
    float t = (abs(denom) < 1.0e-10f) ? 0.5f : clamp((iso - v0) / denom, 0.0f, 1.0f);
    return mix(p0, p1, t);
}

// Compute gradient normal at a world position via central differences.
// Samples the volume at +/- one texel in each axis.
inline float3 computeGradientNormal(texture3d<float> volume, sampler s,
                                    float3 worldPos,
                                    float3 domainMin, float3 invDomainSize,
                                    float3 invDims, float3 halfTexel,
                                    bool flipNormals)
{
    // Convert to UVW space, offset by half-texel in each direction
    float3 uvw = (worldPos - domainMin) * invDomainSize;

    float dx = volume.sample(s, uvw + float3(halfTexel.x, 0, 0)).x
             - volume.sample(s, uvw - float3(halfTexel.x, 0, 0)).x;
    float dy = volume.sample(s, uvw + float3(0, halfTexel.y, 0)).x
             - volume.sample(s, uvw - float3(0, halfTexel.y, 0)).x;
    float dz = volume.sample(s, uvw + float3(0, 0, halfTexel.z)).x
             - volume.sample(s, uvw - float3(0, 0, halfTexel.z)).x;

    // Gradient points low→high; normals should point outward (high→low)
    float3 n = flipNormals ? float3(dx, dy, dz) : float3(-dx, -dy, -dz);

    float len = length(n);
    return (len > 1.0e-8f) ? (n / len) : float3(0.0f, 1.0f, 0.0f);
}

// Compute tangent from normal.
inline float4 computeTangent(float3 normal)
{
    // Cross normal with world-up (0,1,0)
    float3 c = cross(normal, float3(0.0f, 1.0f, 0.0f));
    float len = length(c);

    if (len < 1.0e-6f) {
        // Normal parallel to up -- use right axis
        c = cross(normal, float3(1.0f, 0.0f, 0.0f));
        len = length(c);
    }

    float3 t = (len > 1.0e-6f) ? (c / len) : float3(1.0f, 0.0f, 0.0f);
    return float4(t, 1.0f); // handedness = 1.0
}

// ── Pass 1: Classify cells and count total vertices ─────────────────

kernel void classifyCells(
    texture3d<float>        volume       [[texture(0)]],
    sampler                 fieldSampler [[sampler(0)]],
    constant MCParams&      params       [[buffer(0)]],
    constant ushort*        edgeTable    [[buffer(1)]],
    constant char*          triTable     [[buffer(2)]],
    device atomic_uint*     vertexCount  [[buffer(3)]],
    uint3                   gid          [[thread_position_in_grid]])
{
    // Each thread processes one cell. Grid covers (dims-1) cells per axis.
    if (gid.x >= params.dimsX - 1 ||
        gid.y >= params.dimsY - 1 ||
        gid.z >= params.dimsZ - 1) return;

    float3 invDims = float3(1.0f / float(params.dimsX),
                            1.0f / float(params.dimsY),
                            1.0f / float(params.dimsZ));

    // Sample 8 corner values (Lorensen & Cline convention)
    int x = int(gid.x);
    int y = int(gid.y);
    int z = int(gid.z);

    float v0 = sampleVoxel(volume, fieldSampler, x,   y,   z,   invDims);
    float v1 = sampleVoxel(volume, fieldSampler, x+1, y,   z,   invDims);
    float v2 = sampleVoxel(volume, fieldSampler, x+1, y+1, z,   invDims);
    float v3 = sampleVoxel(volume, fieldSampler, x,   y+1, z,   invDims);
    float v4 = sampleVoxel(volume, fieldSampler, x,   y,   z+1, invDims);
    float v5 = sampleVoxel(volume, fieldSampler, x+1, y,   z+1, invDims);
    float v6 = sampleVoxel(volume, fieldSampler, x+1, y+1, z+1, invDims);
    float v7 = sampleVoxel(volume, fieldSampler, x,   y+1, z+1, invDims);

    float iso = params.isovalue;

    // Build cube index
    uint cubeIndex = 0;
    if (v0 >= iso) cubeIndex |= 1u;
    if (v1 >= iso) cubeIndex |= 2u;
    if (v2 >= iso) cubeIndex |= 4u;
    if (v3 >= iso) cubeIndex |= 8u;
    if (v4 >= iso) cubeIndex |= 16u;
    if (v5 >= iso) cubeIndex |= 32u;
    if (v6 >= iso) cubeIndex |= 64u;
    if (v7 >= iso) cubeIndex |= 128u;

    // Skip if entirely inside or outside
    ushort edges = edgeTable[cubeIndex];
    if (edges == 0) return;

    // Count vertices from triangle table
    uint numVerts = 0;
    for (int i = 0; i < 16; i += 3) {
        if (triTable[cubeIndex * 16 + i] == -1) break;
        numVerts += 3;
    }

    if (numVerts > 0) {
        atomic_fetch_add_explicit(vertexCount, numVerts, memory_order_relaxed);
    }
}

// ── Pass 2: Generate vertices with atomic allocation ────────────────

kernel void generateVertices(
    texture3d<float>        volume       [[texture(0)]],
    sampler                 fieldSampler [[sampler(0)]],
    constant MCParams&      params       [[buffer(0)]],
    constant ushort*        edgeTable    [[buffer(1)]],
    constant char*          triTable     [[buffer(2)]],
    device atomic_uint*     vertexCount  [[buffer(3)]],
    device VertexData*      vertices     [[buffer(4)]],
    uint3                   gid          [[thread_position_in_grid]])
{
    if (gid.x >= params.dimsX - 1 ||
        gid.y >= params.dimsY - 1 ||
        gid.z >= params.dimsZ - 1) return;

    float3 invDims = float3(1.0f / float(params.dimsX),
                            1.0f / float(params.dimsY),
                            1.0f / float(params.dimsZ));

    int x = int(gid.x);
    int y = int(gid.y);
    int z = int(gid.z);

    // Sample 8 corners
    float v0 = sampleVoxel(volume, fieldSampler, x,   y,   z,   invDims);
    float v1 = sampleVoxel(volume, fieldSampler, x+1, y,   z,   invDims);
    float v2 = sampleVoxel(volume, fieldSampler, x+1, y+1, z,   invDims);
    float v3 = sampleVoxel(volume, fieldSampler, x,   y+1, z,   invDims);
    float v4 = sampleVoxel(volume, fieldSampler, x,   y,   z+1, invDims);
    float v5 = sampleVoxel(volume, fieldSampler, x+1, y,   z+1, invDims);
    float v6 = sampleVoxel(volume, fieldSampler, x+1, y+1, z+1, invDims);
    float v7 = sampleVoxel(volume, fieldSampler, x,   y+1, z+1, invDims);

    float iso = params.isovalue;

    uint cubeIndex = 0;
    if (v0 >= iso) cubeIndex |= 1u;
    if (v1 >= iso) cubeIndex |= 2u;
    if (v2 >= iso) cubeIndex |= 4u;
    if (v3 >= iso) cubeIndex |= 8u;
    if (v4 >= iso) cubeIndex |= 16u;
    if (v5 >= iso) cubeIndex |= 32u;
    if (v6 >= iso) cubeIndex |= 64u;
    if (v7 >= iso) cubeIndex |= 128u;

    ushort edges = edgeTable[cubeIndex];
    if (edges == 0) return;

    // Corner world positions
    float3 domainMin = float3(params.domainMinX, params.domainMinY, params.domainMinZ);
    float3 domainMax = float3(params.domainMaxX, params.domainMaxY, params.domainMaxZ);
    float3 texelSize = float3(params.texelSizeX, params.texelSizeY, params.texelSizeZ);
    float3 domainSize = domainMax - domainMin;

    float3 p0 = domainMin + float3(x,   y,   z  ) * texelSize * domainSize;
    float3 p1 = domainMin + float3(x+1, y,   z  ) * texelSize * domainSize;
    float3 p2 = domainMin + float3(x+1, y+1, z  ) * texelSize * domainSize;
    float3 p3 = domainMin + float3(x,   y+1, z  ) * texelSize * domainSize;
    float3 p4 = domainMin + float3(x,   y,   z+1) * texelSize * domainSize;
    float3 p5 = domainMin + float3(x+1, y,   z+1) * texelSize * domainSize;
    float3 p6 = domainMin + float3(x+1, y+1, z+1) * texelSize * domainSize;
    float3 p7 = domainMin + float3(x,   y+1, z+1) * texelSize * domainSize;

    // Compute interpolated edge vertices
    float3 edgeVerts[12];
    if (edges & 0x001) edgeVerts[0]  = interpolateEdge(p0, p1, v0, v1, iso);
    if (edges & 0x002) edgeVerts[1]  = interpolateEdge(p1, p2, v1, v2, iso);
    if (edges & 0x004) edgeVerts[2]  = interpolateEdge(p2, p3, v2, v3, iso);
    if (edges & 0x008) edgeVerts[3]  = interpolateEdge(p3, p0, v3, v0, iso);
    if (edges & 0x010) edgeVerts[4]  = interpolateEdge(p4, p5, v4, v5, iso);
    if (edges & 0x020) edgeVerts[5]  = interpolateEdge(p5, p6, v5, v6, iso);
    if (edges & 0x040) edgeVerts[6]  = interpolateEdge(p6, p7, v6, v7, iso);
    if (edges & 0x080) edgeVerts[7]  = interpolateEdge(p7, p4, v7, v4, iso);
    if (edges & 0x100) edgeVerts[8]  = interpolateEdge(p0, p4, v0, v4, iso);
    if (edges & 0x200) edgeVerts[9]  = interpolateEdge(p1, p5, v1, v5, iso);
    if (edges & 0x400) edgeVerts[10] = interpolateEdge(p2, p6, v2, v6, iso);
    if (edges & 0x800) edgeVerts[11] = interpolateEdge(p3, p7, v3, v7, iso);

    // Gradient normal helpers
    float3 invDomainSize = float3(1.0f / domainSize.x,
                                  1.0f / domainSize.y,
                                  1.0f / domainSize.z);
    float3 halfTexel = 0.5f * invDims;
    bool doFlip = (params.flipNormals != 0);

    // Count vertices first to do a single atomic allocation
    uint numVerts = 0;
    for (int i = 0; i < 16; i += 3) {
        if (triTable[cubeIndex * 16 + i] == -1) break;
        numVerts += 3;
    }

    if (numVerts == 0) return;

    // Atomically allocate space in the output buffer
    uint baseIdx = atomic_fetch_add_explicit(vertexCount, numVerts, memory_order_relaxed);

    // Safety check: don't write past buffer end
    if (baseIdx + numVerts > params.maxVertices) return;

    // Emit triangles
    uint writeIdx = baseIdx;
    for (int i = 0; i < 16; i += 3) {
        int e0 = triTable[cubeIndex * 16 + i];
        if (e0 == -1) break;
        int e1 = triTable[cubeIndex * 16 + i + 1];
        int e2 = triTable[cubeIndex * 16 + i + 2];

        float3 positions[3] = { edgeVerts[e0], edgeVerts[e1], edgeVerts[e2] };

        for (int vi = 0; vi < 3; ++vi) {
            float3 pos = positions[vi];
            float3 normal = computeGradientNormal(volume, fieldSampler,
                                                   pos, domainMin, invDomainSize,
                                                   invDims, halfTexel, doFlip);
            float4 tangent = computeTangent(normal);

            VertexData vd;
            vd.position = packed_float3(pos);
            vd.normal   = packed_float3(normal);
            vd.uv0      = packed_float2(0.0f, 0.0f);
            vd.tangent   = packed_float4(tangent);
            vd.uv1      = packed_float2(0.0f, 0.0f);

            vertices[writeIdx] = vd;
            ++writeIdx;
        }
    }
}
)";

        constexpr uint32_t THREADGROUP_SIZE_X = 4;
        constexpr uint32_t THREADGROUP_SIZE_Y = 4;
        constexpr uint32_t THREADGROUP_SIZE_Z = 4;

        // ── Marching Cubes Lookup Tables ────────────────────────────────
        // Identical to viz/algorithms/marchingCubes.h (Lorensen & Cline convention).

        constexpr uint16_t kEdgeTable[256] = {
            0x000, 0x109, 0x203, 0x30A, 0x406, 0x50F, 0x605, 0x70C,
            0x80C, 0x905, 0xA0F, 0xB06, 0xC0A, 0xD03, 0xE09, 0xF00,
            0x190, 0x099, 0x393, 0x29A, 0x596, 0x49F, 0x795, 0x69C,
            0x99C, 0x895, 0xB9F, 0xA96, 0xD9A, 0xC93, 0xF99, 0xE90,
            0x230, 0x339, 0x033, 0x13A, 0x636, 0x73F, 0x435, 0x53C,
            0xA3C, 0xB35, 0x83F, 0x936, 0xE3A, 0xF33, 0xC39, 0xD30,
            0x3A0, 0x2A9, 0x1A3, 0x0AA, 0x7A6, 0x6AF, 0x5A5, 0x4AC,
            0xBAC, 0xAA5, 0x9AF, 0x8A6, 0xFAA, 0xEA3, 0xDA9, 0xCA0,
            0x460, 0x569, 0x663, 0x76A, 0x066, 0x16F, 0x265, 0x36C,
            0xC6C, 0xD65, 0xE6F, 0xF66, 0x86A, 0x963, 0xA69, 0xB60,
            0x5F0, 0x4F9, 0x7F3, 0x6FA, 0x1F6, 0x0FF, 0x3F5, 0x2FC,
            0xDFC, 0xCF5, 0xFFF, 0xEF6, 0x9FA, 0x8F3, 0xBF9, 0xAF0,
            0x650, 0x759, 0x453, 0x55A, 0x256, 0x35F, 0x055, 0x15C,
            0xE5C, 0xF55, 0xC5F, 0xD56, 0xA5A, 0xB53, 0x859, 0x950,
            0x7C0, 0x6C9, 0x5C3, 0x4CA, 0x3C6, 0x2CF, 0x1C5, 0x0CC,
            0xFCC, 0xEC5, 0xDCF, 0xCC6, 0xBCA, 0xAC3, 0x9C9, 0x8C0,
            0x8C0, 0x9C9, 0xAC3, 0xBCA, 0xCC6, 0xDCF, 0xEC5, 0xFCC,
            0x0CC, 0x1C5, 0x2CF, 0x3C6, 0x4CA, 0x5C3, 0x6C9, 0x7C0,
            0x950, 0x859, 0xB53, 0xA5A, 0xD56, 0xC5F, 0xF55, 0xE5C,
            0x15C, 0x055, 0x35F, 0x256, 0x55A, 0x453, 0x759, 0x650,
            0xAF0, 0xBF9, 0x8F3, 0x9FA, 0xEF6, 0xFFF, 0xCF5, 0xDFC,
            0x2FC, 0x3F5, 0x0FF, 0x1F6, 0x6FA, 0x7F3, 0x4F9, 0x5F0,
            0xB60, 0xA69, 0x963, 0x86A, 0xF66, 0xE6F, 0xD65, 0xC6C,
            0x36C, 0x265, 0x16F, 0x066, 0x76A, 0x663, 0x569, 0x460,
            0xCA0, 0xDA9, 0xEA3, 0xFAA, 0x8A6, 0x9AF, 0xAA5, 0xBAC,
            0x4AC, 0x5A5, 0x6AF, 0x7A6, 0x0AA, 0x1A3, 0x2A9, 0x3A0,
            0xD30, 0xC39, 0xF33, 0xE3A, 0x936, 0x83F, 0xB35, 0xA3C,
            0x53C, 0x435, 0x73F, 0x636, 0x13A, 0x033, 0x339, 0x230,
            0xE90, 0xF99, 0xC93, 0xD9A, 0xA96, 0xB9F, 0x895, 0x99C,
            0x69C, 0x795, 0x49F, 0x596, 0x29A, 0x393, 0x099, 0x190,
            0xF00, 0xE09, 0xD03, 0xC0A, 0xB06, 0xA0F, 0x905, 0x80C,
            0x70C, 0x605, 0x50F, 0x406, 0x30A, 0x203, 0x109, 0x000
        };

        constexpr int8_t kTriTable[256][16] = {
            {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8,  3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  1,  9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  8,  3,  9,  8,  1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8,  3,  1,  2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 9,  2, 10,  0,  2,  9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 2,  8,  3,  2, 10,  8, 10,  9,  8, -1, -1, -1, -1, -1, -1, -1},
            { 3, 11,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0, 11,  2,  8, 11,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  9,  0,  2,  3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1, 11,  2,  1,  9, 11,  9,  8, 11, -1, -1, -1, -1, -1, -1, -1},
            { 3, 10,  1, 11, 10,  3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0, 10,  1,  0,  8, 10,  8, 11, 10, -1, -1, -1, -1, -1, -1, -1},
            { 3,  9,  0,  3, 11,  9, 11, 10,  9, -1, -1, -1, -1, -1, -1, -1},
            { 9,  8, 10, 10,  8, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4,  3,  0,  7,  3,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  1,  9,  8,  4,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4,  1,  9,  4,  7,  1,  7,  3,  1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2, 10,  8,  4,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 3,  4,  7,  3,  0,  4,  1,  2, 10, -1, -1, -1, -1, -1, -1, -1},
            { 9,  2, 10,  9,  0,  2,  8,  4,  7, -1, -1, -1, -1, -1, -1, -1},
            { 2, 10,  9,  2,  9,  7,  2,  7,  3,  7,  9,  4, -1, -1, -1, -1},
            { 8,  4,  7,  3, 11,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            {11,  4,  7, 11,  2,  4,  2,  0,  4, -1, -1, -1, -1, -1, -1, -1},
            { 9,  0,  1,  8,  4,  7,  2,  3, 11, -1, -1, -1, -1, -1, -1, -1},
            { 4,  7, 11,  9,  4, 11,  9, 11,  2,  9,  2,  1, -1, -1, -1, -1},
            { 3, 10,  1,  3, 11, 10,  7,  8,  4, -1, -1, -1, -1, -1, -1, -1},
            { 1, 11, 10,  1,  4, 11,  1,  0,  4,  7, 11,  4, -1, -1, -1, -1},
            { 4,  7,  8,  9,  0, 11,  9, 11, 10, 11,  0,  3, -1, -1, -1, -1},
            { 4,  7, 11,  4, 11,  9,  9, 11, 10, -1, -1, -1, -1, -1, -1, -1},
            { 9,  5,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 9,  5,  4,  0,  8,  3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  5,  4,  1,  5,  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 8,  5,  4,  8,  3,  5,  3,  1,  5, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2, 10,  9,  5,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 3,  0,  8,  1,  2, 10,  4,  9,  5, -1, -1, -1, -1, -1, -1, -1},
            { 5,  2, 10,  5,  4,  2,  4,  0,  2, -1, -1, -1, -1, -1, -1, -1},
            { 2, 10,  5,  3,  2,  5,  3,  5,  4,  3,  4,  8, -1, -1, -1, -1},
            { 9,  5,  4,  2,  3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0, 11,  2,  0,  8, 11,  4,  9,  5, -1, -1, -1, -1, -1, -1, -1},
            { 0,  5,  4,  0,  1,  5,  2,  3, 11, -1, -1, -1, -1, -1, -1, -1},
            { 2,  1,  5,  2,  5,  8,  2,  8, 11,  4,  8,  5, -1, -1, -1, -1},
            {10,  3, 11, 10,  1,  3,  9,  5,  4, -1, -1, -1, -1, -1, -1, -1},
            { 4,  9,  5,  0,  8,  1,  8, 10,  1,  8, 11, 10, -1, -1, -1, -1},
            { 5,  4,  0,  5,  0, 11,  5, 11, 10, 11,  0,  3, -1, -1, -1, -1},
            { 5,  4,  8,  5,  8, 10, 10,  8, 11, -1, -1, -1, -1, -1, -1, -1},
            { 9,  7,  8,  5,  7,  9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 9,  3,  0,  9,  5,  3,  5,  7,  3, -1, -1, -1, -1, -1, -1, -1},
            { 0,  7,  8,  0,  1,  7,  1,  5,  7, -1, -1, -1, -1, -1, -1, -1},
            { 1,  5,  3,  3,  5,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 9,  7,  8,  9,  5,  7, 10,  1,  2, -1, -1, -1, -1, -1, -1, -1},
            {10,  1,  2,  9,  5,  0,  5,  3,  0,  5,  7,  3, -1, -1, -1, -1},
            { 8,  0,  2,  8,  2,  5,  8,  5,  7, 10,  5,  2, -1, -1, -1, -1},
            { 2, 10,  5,  2,  5,  3,  3,  5,  7, -1, -1, -1, -1, -1, -1, -1},
            { 7,  9,  5,  7,  8,  9,  3, 11,  2, -1, -1, -1, -1, -1, -1, -1},
            { 9,  5,  7,  9,  7,  2,  9,  2,  0,  2,  7, 11, -1, -1, -1, -1},
            { 2,  3, 11,  0,  1,  8,  1,  7,  8,  1,  5,  7, -1, -1, -1, -1},
            {11,  2,  1, 11,  1,  7,  7,  1,  5, -1, -1, -1, -1, -1, -1, -1},
            { 9,  5,  8,  8,  5,  7, 10,  1,  3, 10,  3, 11, -1, -1, -1, -1},
            { 5,  7,  0,  5,  0,  9,  7, 11,  0,  1,  0, 10, 11, 10,  0, -1},
            {11, 10,  0, 11,  0,  3, 10,  5,  0,  8,  0,  7,  5,  7,  0, -1},
            {11, 10,  5,  7, 11,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            {10,  6,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8,  3,  5, 10,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 9,  0,  1,  5, 10,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  8,  3,  1,  9,  8,  5, 10,  6, -1, -1, -1, -1, -1, -1, -1},
            { 1,  6,  5,  2,  6,  1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  6,  5,  1,  2,  6,  3,  0,  8, -1, -1, -1, -1, -1, -1, -1},
            { 9,  6,  5,  9,  0,  6,  0,  2,  6, -1, -1, -1, -1, -1, -1, -1},
            { 5,  9,  8,  5,  8,  2,  5,  2,  6,  3,  2,  8, -1, -1, -1, -1},
            { 2,  3, 11, 10,  6,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            {11,  0,  8, 11,  2,  0, 10,  6,  5, -1, -1, -1, -1, -1, -1, -1},
            { 0,  1,  9,  2,  3, 11,  5, 10,  6, -1, -1, -1, -1, -1, -1, -1},
            { 5, 10,  6,  1,  9,  2,  9, 11,  2,  9,  8, 11, -1, -1, -1, -1},
            { 6,  3, 11,  6,  5,  3,  5,  1,  3, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8, 11,  0, 11,  5,  0,  5,  1,  5, 11,  6, -1, -1, -1, -1},
            { 3, 11,  6,  0,  3,  6,  0,  6,  5,  0,  5,  9, -1, -1, -1, -1},
            { 6,  5,  9,  6,  9, 11, 11,  9,  8, -1, -1, -1, -1, -1, -1, -1},
            { 5, 10,  6,  4,  7,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4,  3,  0,  4,  7,  3,  6,  5, 10, -1, -1, -1, -1, -1, -1, -1},
            { 1,  9,  0,  5, 10,  6,  8,  4,  7, -1, -1, -1, -1, -1, -1, -1},
            {10,  6,  5,  1,  9,  7,  1,  7,  3,  7,  9,  4, -1, -1, -1, -1},
            { 6,  1,  2,  6,  5,  1,  4,  7,  8, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2,  5,  5,  2,  6,  3,  0,  4,  3,  4,  7, -1, -1, -1, -1},
            { 8,  4,  7,  9,  0,  5,  0,  6,  5,  0,  2,  6, -1, -1, -1, -1},
            { 7,  3,  9,  7,  9,  4,  3,  2,  9,  5,  9,  6,  2,  6,  9, -1},
            { 3, 11,  2,  7,  8,  4, 10,  6,  5, -1, -1, -1, -1, -1, -1, -1},
            { 5, 10,  6,  4,  7,  2,  4,  2,  0,  2,  7, 11, -1, -1, -1, -1},
            { 0,  1,  9,  4,  7,  8,  2,  3, 11,  5, 10,  6, -1, -1, -1, -1},
            { 9,  2,  1,  9, 11,  2,  9,  4, 11,  7, 11,  4,  5, 10,  6, -1},
            { 8,  4,  7,  3, 11,  5,  3,  5,  1,  5, 11,  6, -1, -1, -1, -1},
            { 5,  1, 11,  5, 11,  6,  1,  0, 11,  7, 11,  4,  0,  4, 11, -1},
            { 0,  5,  9,  0,  6,  5,  0,  3,  6, 11,  6,  3,  8,  4,  7, -1},
            { 6,  5,  9,  6,  9, 11,  4,  7,  9,  7, 11,  9, -1, -1, -1, -1},
            {10,  4,  9,  6,  4, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4, 10,  6,  4,  9, 10,  0,  8,  3, -1, -1, -1, -1, -1, -1, -1},
            {10,  0,  1, 10,  6,  0,  6,  4,  0, -1, -1, -1, -1, -1, -1, -1},
            { 8,  3,  1,  8,  1,  6,  8,  6,  4,  6,  1, 10, -1, -1, -1, -1},
            { 1,  4,  9,  1,  2,  4,  2,  6,  4, -1, -1, -1, -1, -1, -1, -1},
            { 3,  0,  8,  1,  2,  9,  2,  4,  9,  2,  6,  4, -1, -1, -1, -1},
            { 0,  2,  4,  4,  2,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 8,  3,  2,  8,  2,  4,  4,  2,  6, -1, -1, -1, -1, -1, -1, -1},
            {10,  4,  9, 10,  6,  4, 11,  2,  3, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8,  2,  2,  8, 11,  4,  9, 10,  4, 10,  6, -1, -1, -1, -1},
            { 3, 11,  2,  0,  1,  6,  0,  6,  4,  6,  1, 10, -1, -1, -1, -1},
            { 6,  4,  1,  6,  1, 10,  4,  8,  1,  2,  1, 11,  8, 11,  1, -1},
            { 9,  6,  4,  9,  3,  6,  9,  1,  3, 11,  6,  3, -1, -1, -1, -1},
            { 8, 11,  1,  8,  1,  0, 11,  6,  1,  9,  1,  4,  6,  4,  1, -1},
            { 3, 11,  6,  3,  6,  0,  0,  6,  4, -1, -1, -1, -1, -1, -1, -1},
            { 6,  4,  8, 11,  6,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 7, 10,  6,  7,  8, 10,  8,  9, 10, -1, -1, -1, -1, -1, -1, -1},
            { 0,  7,  3,  0, 10,  7,  0,  9, 10,  6,  7, 10, -1, -1, -1, -1},
            {10,  6,  7,  1, 10,  7,  1,  7,  8,  1,  8,  0, -1, -1, -1, -1},
            {10,  6,  7, 10,  7,  1,  1,  7,  3, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2,  6,  1,  6,  8,  1,  8,  9,  8,  6,  7, -1, -1, -1, -1},
            { 2,  6,  9,  2,  9,  1,  6,  7,  9,  0,  9,  3,  7,  3,  9, -1},
            { 7,  8,  0,  7,  0,  6,  6,  0,  2, -1, -1, -1, -1, -1, -1, -1},
            { 7,  3,  2,  6,  7,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 2,  3, 11, 10,  6,  8, 10,  8,  9,  8,  6,  7, -1, -1, -1, -1},
            { 2,  0,  7,  2,  7, 11,  0,  9,  7,  6,  7, 10,  9, 10,  7, -1},
            { 1,  8,  0,  1,  7,  8,  1, 10,  7,  6,  7, 10,  2,  3, 11, -1},
            {11,  2,  1, 11,  1,  7, 10,  6,  1,  6,  7,  1, -1, -1, -1, -1},
            { 8,  9,  6,  8,  6,  7,  9,  1,  6, 11,  6,  3,  1,  3,  6, -1},
            { 0,  9,  1, 11,  6,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 7,  8,  0,  7,  0,  6,  3, 11,  0, 11,  6,  0, -1, -1, -1, -1},
            { 7, 11,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 7,  6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 3,  0,  8, 11,  7,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  1,  9, 11,  7,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 8,  1,  9,  8,  3,  1, 11,  7,  6, -1, -1, -1, -1, -1, -1, -1},
            {10,  1,  2,  6, 11,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2, 10,  3,  0,  8,  6, 11,  7, -1, -1, -1, -1, -1, -1, -1},
            { 2,  9,  0,  2, 10,  9,  6, 11,  7, -1, -1, -1, -1, -1, -1, -1},
            { 6, 11,  7,  2, 10,  3, 10,  8,  3, 10,  9,  8, -1, -1, -1, -1},
            { 7,  2,  3,  6,  2,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 7,  0,  8,  7,  6,  0,  6,  2,  0, -1, -1, -1, -1, -1, -1, -1},
            { 2,  7,  6,  2,  3,  7,  0,  1,  9, -1, -1, -1, -1, -1, -1, -1},
            { 1,  6,  2,  1,  8,  6,  1,  9,  8,  8,  7,  6, -1, -1, -1, -1},
            {10,  7,  6, 10,  1,  7,  1,  3,  7, -1, -1, -1, -1, -1, -1, -1},
            {10,  7,  6,  1,  7, 10,  1,  8,  7,  1,  0,  8, -1, -1, -1, -1},
            { 0,  3,  7,  0,  7, 10,  0, 10,  9,  6, 10,  7, -1, -1, -1, -1},
            { 7,  6, 10,  7, 10,  8,  8, 10,  9, -1, -1, -1, -1, -1, -1, -1},
            { 6,  8,  4, 11,  8,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 3,  6, 11,  3,  0,  6,  0,  4,  6, -1, -1, -1, -1, -1, -1, -1},
            { 8,  6, 11,  8,  4,  6,  9,  0,  1, -1, -1, -1, -1, -1, -1, -1},
            { 9,  4,  6,  9,  6,  3,  9,  3,  1, 11,  3,  6, -1, -1, -1, -1},
            { 6,  8,  4,  6, 11,  8,  2, 10,  1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2, 10,  3,  0, 11,  0,  6, 11,  0,  4,  6, -1, -1, -1, -1},
            { 4, 11,  8,  4,  6, 11,  0,  2,  9,  2, 10,  9, -1, -1, -1, -1},
            {10,  9,  3, 10,  3,  2,  9,  4,  3, 11,  3,  6,  4,  6,  3, -1},
            { 8,  2,  3,  8,  4,  2,  4,  6,  2, -1, -1, -1, -1, -1, -1, -1},
            { 0,  4,  2,  4,  6,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  9,  0,  2,  3,  4,  2,  4,  6,  4,  3,  8, -1, -1, -1, -1},
            { 1,  9,  4,  1,  4,  2,  2,  4,  6, -1, -1, -1, -1, -1, -1, -1},
            { 8,  1,  3,  8,  6,  1,  8,  4,  6,  6, 10,  1, -1, -1, -1, -1},
            {10,  1,  0, 10,  0,  6,  6,  0,  4, -1, -1, -1, -1, -1, -1, -1},
            { 4,  6,  3,  4,  3,  8,  6, 10,  3,  0,  3,  9, 10,  9,  3, -1},
            {10,  9,  4,  6, 10,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4,  9,  5,  7,  6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8,  3,  4,  9,  5, 11,  7,  6, -1, -1, -1, -1, -1, -1, -1},
            { 5,  0,  1,  5,  4,  0,  7,  6, 11, -1, -1, -1, -1, -1, -1, -1},
            {11,  7,  6,  8,  3,  4,  3,  5,  4,  3,  1,  5, -1, -1, -1, -1},
            { 9,  5,  4, 10,  1,  2,  7,  6, 11, -1, -1, -1, -1, -1, -1, -1},
            { 6, 11,  7,  1,  2, 10,  0,  8,  3,  4,  9,  5, -1, -1, -1, -1},
            { 7,  6, 11,  5,  4, 10,  4,  2, 10,  4,  0,  2, -1, -1, -1, -1},
            { 3,  4,  8,  3,  5,  4,  3,  2,  5, 10,  5,  2, 11,  7,  6, -1},
            { 7,  2,  3,  7,  6,  2,  5,  4,  9, -1, -1, -1, -1, -1, -1, -1},
            { 9,  5,  4,  0,  8,  6,  0,  6,  2,  6,  8,  7, -1, -1, -1, -1},
            { 3,  6,  2,  3,  7,  6,  1,  5,  0,  5,  4,  0, -1, -1, -1, -1},
            { 6,  2,  8,  6,  8,  7,  2,  1,  8,  4,  8,  5,  1,  5,  8, -1},
            { 9,  5,  4, 10,  1,  6,  1,  7,  6,  1,  3,  7, -1, -1, -1, -1},
            { 1,  6, 10,  1,  7,  6,  1,  0,  7,  8,  7,  0,  9,  5,  4, -1},
            { 4,  0, 10,  4, 10,  5,  0,  3, 10,  6, 10,  7,  3,  7, 10, -1},
            { 7,  6, 10,  7, 10,  8,  5,  4, 10,  4,  8, 10, -1, -1, -1, -1},
            { 6,  9,  5,  6, 11,  9, 11,  8,  9, -1, -1, -1, -1, -1, -1, -1},
            { 3,  6, 11,  0,  6,  3,  0,  5,  6,  0,  9,  5, -1, -1, -1, -1},
            { 0, 11,  8,  0,  5, 11,  0,  1,  5,  5,  6, 11, -1, -1, -1, -1},
            { 6, 11,  3,  6,  3,  5,  5,  3,  1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2, 10,  9,  5, 11,  9, 11,  8, 11,  5,  6, -1, -1, -1, -1},
            { 0, 11,  3,  0,  6, 11,  0,  9,  6,  5,  6,  9,  1,  2, 10, -1},
            {11,  8,  5, 11,  5,  6,  8,  0,  5, 10,  5,  2,  0,  2,  5, -1},
            { 6, 11,  3,  6,  3,  5,  2, 10,  3, 10,  5,  3, -1, -1, -1, -1},
            { 5,  8,  9,  5,  2,  8,  5,  6,  2,  3,  8,  2, -1, -1, -1, -1},
            { 9,  5,  6,  9,  6,  0,  0,  6,  2, -1, -1, -1, -1, -1, -1, -1},
            { 1,  5,  8,  1,  8,  0,  5,  6,  8,  3,  8,  2,  6,  2,  8, -1},
            { 1,  5,  6,  2,  1,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  3,  6,  1,  6, 10,  3,  8,  6,  5,  6,  9,  8,  9,  6, -1},
            {10,  1,  0, 10,  0,  6,  9,  5,  0,  5,  6,  0, -1, -1, -1, -1},
            { 0,  3,  8,  5,  6, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            {10,  5,  6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            {11,  5, 10,  7,  5, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            {11,  5, 10, 11,  7,  5,  8,  3,  0, -1, -1, -1, -1, -1, -1, -1},
            { 5, 11,  7,  5, 10, 11,  1,  9,  0, -1, -1, -1, -1, -1, -1, -1},
            {10,  7,  5, 10, 11,  7,  9,  8,  1,  8,  3,  1, -1, -1, -1, -1},
            {11,  1,  2, 11,  7,  1,  7,  5,  1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8,  3,  1,  2,  7,  1,  7,  5,  7,  2, 11, -1, -1, -1, -1},
            { 9,  7,  5,  9,  2,  7,  9,  0,  2,  2, 11,  7, -1, -1, -1, -1},
            { 7,  5,  2,  7,  2, 11,  5,  9,  2,  3,  2,  8,  9,  8,  2, -1},
            { 2,  5, 10,  2,  3,  5,  3,  7,  5, -1, -1, -1, -1, -1, -1, -1},
            { 8,  2,  0,  8,  5,  2,  8,  7,  5, 10,  2,  5, -1, -1, -1, -1},
            { 9,  0,  1,  5, 10,  3,  5,  3,  7,  3, 10,  2, -1, -1, -1, -1},
            { 9,  8,  2,  9,  2,  1,  8,  7,  2, 10,  2,  5,  7,  5,  2, -1},
            { 1,  3,  5,  3,  7,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8,  7,  0,  7,  1,  1,  7,  5, -1, -1, -1, -1, -1, -1, -1},
            { 9,  0,  3,  9,  3,  5,  5,  3,  7, -1, -1, -1, -1, -1, -1, -1},
            { 9,  8,  7,  5,  9,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 5,  8,  4,  5, 10,  8, 10, 11,  8, -1, -1, -1, -1, -1, -1, -1},
            { 5,  0,  4,  5, 11,  0,  5, 10, 11, 11,  3,  0, -1, -1, -1, -1},
            { 0,  1,  9,  8,  4, 10,  8, 10, 11, 10,  4,  5, -1, -1, -1, -1},
            {10, 11,  4, 10,  4,  5, 11,  3,  4,  9,  4,  1,  3,  1,  4, -1},
            { 2,  5,  1,  2,  8,  5,  2, 11,  8,  4,  5,  8, -1, -1, -1, -1},
            { 0,  4, 11,  0, 11,  3,  4,  5, 11,  2, 11,  1,  5,  1, 11, -1},
            { 0,  2,  5,  0,  5,  9,  2, 11,  5,  4,  5,  8, 11,  8,  5, -1},
            { 9,  4,  5,  2, 11,  3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 2,  5, 10,  3,  5,  2,  3,  4,  5,  3,  8,  4, -1, -1, -1, -1},
            { 5, 10,  2,  5,  2,  4,  4,  2,  0, -1, -1, -1, -1, -1, -1, -1},
            { 3, 10,  2,  3,  5, 10,  3,  8,  5,  4,  5,  8,  0,  1,  9, -1},
            { 5, 10,  2,  5,  2,  4,  1,  9,  2,  9,  4,  2, -1, -1, -1, -1},
            { 8,  4,  5,  8,  5,  3,  3,  5,  1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  4,  5,  1,  0,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 8,  4,  5,  8,  5,  3,  9,  0,  5,  0,  3,  5, -1, -1, -1, -1},
            { 9,  4,  5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4, 11,  7,  4,  9, 11,  9, 10, 11, -1, -1, -1, -1, -1, -1, -1},
            { 0,  8,  3,  4,  9,  7,  9, 11,  7,  9, 10, 11, -1, -1, -1, -1},
            { 1, 10, 11,  1, 11,  4,  1,  4,  0,  7,  4, 11, -1, -1, -1, -1},
            { 3,  1,  4,  3,  4,  8,  1, 10,  4,  7,  4, 11, 10, 11,  4, -1},
            { 4, 11,  7,  9, 11,  4,  9,  2, 11,  9,  1,  2, -1, -1, -1, -1},
            { 9,  7,  4,  9, 11,  7,  9,  1, 11,  2, 11,  1,  0,  8,  3, -1},
            {11,  7,  4, 11,  4,  2,  2,  4,  0, -1, -1, -1, -1, -1, -1, -1},
            {11,  7,  4, 11,  4,  2,  8,  3,  4,  3,  2,  4, -1, -1, -1, -1},
            { 2,  9, 10,  2,  7,  9,  2,  3,  7,  7,  4,  9, -1, -1, -1, -1},
            { 9, 10,  7,  9,  7,  4, 10,  2,  7,  8,  7,  0,  2,  0,  7, -1},
            { 3,  7, 10,  3, 10,  2,  7,  4, 10,  1, 10,  0,  4,  0, 10, -1},
            { 1, 10,  2,  8,  7,  4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4,  9,  1,  4,  1,  7,  7,  1,  3, -1, -1, -1, -1, -1, -1, -1},
            { 4,  9,  1,  4,  1,  7,  0,  8,  1,  8,  7,  1, -1, -1, -1, -1},
            { 4,  0,  3,  7,  4,  3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 4,  8,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 9, 10,  8, 10, 11,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 3,  0,  9,  3,  9, 11, 11,  9, 10, -1, -1, -1, -1, -1, -1, -1},
            { 0,  1, 10,  0, 10,  8,  8, 10, 11, -1, -1, -1, -1, -1, -1, -1},
            { 3,  1, 10, 11,  3, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  2, 11,  1, 11,  9,  9, 11,  8, -1, -1, -1, -1, -1, -1, -1},
            { 3,  0,  9,  3,  9, 11,  1,  2,  9,  2, 11,  9, -1, -1, -1, -1},
            { 0,  2, 11,  8,  0, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 3,  2, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 2,  3,  8,  2,  8, 10, 10,  8,  9, -1, -1, -1, -1, -1, -1, -1},
            { 9, 10,  2,  0,  9,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 2,  3,  8,  2,  8, 10,  0,  1,  8,  1, 10,  8, -1, -1, -1, -1},
            { 1, 10,  2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 1,  3,  8,  9,  1,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  9,  1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            { 0,  3,  8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
            {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}
        };

    } // anonymous namespace

    // ─── Construction / Destruction ───────────────────────────────────

    MetalMarchingCubesPass::MetalMarchingCubesPass(MetalGraphicsDevice* device)
        : device_(device)
    {
    }

    MetalMarchingCubesPass::~MetalMarchingCubesPass()
    {
        if (classifyPipeline_) { classifyPipeline_->release(); classifyPipeline_ = nullptr; }
        if (generatePipeline_) { generatePipeline_->release(); generatePipeline_ = nullptr; }
        if (edgeTableBuffer_)  { edgeTableBuffer_->release();  edgeTableBuffer_ = nullptr; }
        if (triTableBuffer_)   { triTableBuffer_->release();   triTableBuffer_ = nullptr; }
        if (counterBuffer_)    { counterBuffer_->release();    counterBuffer_ = nullptr; }
        if (uniformBuffer_)    { uniformBuffer_->release();    uniformBuffer_ = nullptr; }
        if (fieldSampler_)     { fieldSampler_->release();     fieldSampler_ = nullptr; }
    }

    // ─── Lazy Resource Creation ──────────────────────────────────────

    void MetalMarchingCubesPass::ensureResources()
    {
        if (resourcesReady_) return;

        auto* mtlDevice = device_->raw();
        if (!mtlDevice) return;

        // ── Compile compute shaders ────────────────────────────────
        if (!classifyPipeline_ || !generatePipeline_) {
            NS::Error* error = nullptr;
            auto* source = NS::String::string(MC_COMPUTE_SOURCE, NS::UTF8StringEncoding);
            auto* library = mtlDevice->newLibrary(source, nullptr, &error);
            if (!library) {
                spdlog::error("[MetalMarchingCubesPass] Failed to compile MC shaders: {}",
                    error ? error->localizedDescription()->utf8String() : "unknown");
                return;
            }

            // Classify pipeline
            if (!classifyPipeline_) {
                auto* funcName = NS::String::string("classifyCells", NS::UTF8StringEncoding);
                auto* function = library->newFunction(funcName);
                if (!function) {
                    spdlog::error("[MetalMarchingCubesPass] Entry point 'classifyCells' not found");
                    library->release();
                    return;
                }
                classifyPipeline_ = mtlDevice->newComputePipelineState(function, &error);
                function->release();
                if (!classifyPipeline_) {
                    spdlog::error("[MetalMarchingCubesPass] Failed to create classify pipeline: {}",
                        error ? error->localizedDescription()->utf8String() : "unknown");
                    library->release();
                    return;
                }
            }

            // Generate pipeline
            if (!generatePipeline_) {
                auto* funcName = NS::String::string("generateVertices", NS::UTF8StringEncoding);
                auto* function = library->newFunction(funcName);
                if (!function) {
                    spdlog::error("[MetalMarchingCubesPass] Entry point 'generateVertices' not found");
                    library->release();
                    return;
                }
                generatePipeline_ = mtlDevice->newComputePipelineState(function, &error);
                function->release();
                if (!generatePipeline_) {
                    spdlog::error("[MetalMarchingCubesPass] Failed to create generate pipeline: {}",
                        error ? error->localizedDescription()->utf8String() : "unknown");
                    library->release();
                    return;
                }
            }

            library->release();
        }

        // ── Lookup table buffers ───────────────────────────────────
        if (!edgeTableBuffer_) {
            edgeTableBuffer_ = mtlDevice->newBuffer(
                kEdgeTable, sizeof(kEdgeTable), MTL::ResourceStorageModeShared);
            if (!edgeTableBuffer_) {
                spdlog::error("[MetalMarchingCubesPass] Failed to create edgeTable buffer");
                return;
            }
        }

        if (!triTableBuffer_) {
            triTableBuffer_ = mtlDevice->newBuffer(
                kTriTable, sizeof(kTriTable), MTL::ResourceStorageModeShared);
            if (!triTableBuffer_) {
                spdlog::error("[MetalMarchingCubesPass] Failed to create triTable buffer");
                return;
            }
        }

        // ── Atomic counter buffer (single uint32) ──────────────────
        if (!counterBuffer_) {
            counterBuffer_ = mtlDevice->newBuffer(
                sizeof(uint32_t), MTL::ResourceStorageModeShared);
            if (!counterBuffer_) {
                spdlog::error("[MetalMarchingCubesPass] Failed to create counter buffer");
                return;
            }
        }

        // ── Uniform buffer (MCComputeParams) ───────────────────────
        if (!uniformBuffer_) {
            uniformBuffer_ = mtlDevice->newBuffer(
                sizeof(MCComputeParams), MTL::ResourceStorageModeShared);
            if (!uniformBuffer_) {
                spdlog::error("[MetalMarchingCubesPass] Failed to create uniform buffer");
                return;
            }
        }

        // ── Nearest-neighbor sampler for exact voxel reads ─────────
        if (!fieldSampler_) {
            auto* desc = MTL::SamplerDescriptor::alloc()->init();
            desc->setMinFilter(MTL::SamplerMinMagFilterNearest);
            desc->setMagFilter(MTL::SamplerMinMagFilterNearest);
            desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
            desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
            desc->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
            fieldSampler_ = mtlDevice->newSamplerState(desc);
            desc->release();
        }

        resourcesReady_ = (classifyPipeline_ && generatePipeline_ &&
                           edgeTableBuffer_ && triTableBuffer_ &&
                           counterBuffer_ && uniformBuffer_ && fieldSampler_);

        if (resourcesReady_) {
            spdlog::info("[MetalMarchingCubesPass] Resources initialized successfully");
        }
    }

    // ─── Per-Extraction Buffer Allocation ───────────────────────────

    MTL::Buffer* MetalMarchingCubesPass::allocateVertexBuffer(uint32_t vertexCount)
    {
        auto* mtlDevice = device_->raw();
        if (!mtlDevice) return nullptr;

        constexpr size_t VERTEX_SIZE = 56; // sizeof(VertexData) in MSL
        const size_t bufferSize = static_cast<size_t>(vertexCount) * VERTEX_SIZE;

        auto* buffer = mtlDevice->newBuffer(bufferSize, MTL::ResourceStorageModeShared);
        if (!buffer) {
            spdlog::error("[MetalMarchingCubesPass] Failed to allocate vertex buffer ({} verts, {:.1f} MB)",
                vertexCount, static_cast<double>(bufferSize) / (1024.0 * 1024.0));
        }

        return buffer;  // Caller takes ownership (newBuffer returns +1 ref)
    }

    // ─── GPU Extraction ──────────────────────────────────────────────

    MCExtractResult MetalMarchingCubesPass::extract(Texture* volumeTexture,
                                                     const MCComputeParams& params)
    {
        MCExtractResult result;

        if (!volumeTexture) {
            spdlog::warn("[MetalMarchingCubesPass] No volume texture provided");
            return result;
        }

        ensureResources();
        if (!resourcesReady_) return result;

        // Get the underlying Metal texture
        auto* hwTexture = dynamic_cast<gpu::MetalTexture*>(volumeTexture->impl());
        if (!hwTexture || !hwTexture->raw()) {
            spdlog::warn("[MetalMarchingCubesPass] Volume texture has no Metal backing");
            return result;
        }

        // Upload uniforms
        std::memcpy(uniformBuffer_->contents(), &params, sizeof(MCComputeParams));

        // Reset atomic counter to 0
        uint32_t zero = 0;
        std::memcpy(counterBuffer_->contents(), &zero, sizeof(uint32_t));

        // Grid dimensions for dispatch (one thread per cell)
        uint32_t cellsX = params.dimsX - 1;
        uint32_t cellsY = params.dimsY - 1;
        uint32_t cellsZ = params.dimsZ - 1;

        auto threadgroupsX = (cellsX + THREADGROUP_SIZE_X - 1) / THREADGROUP_SIZE_X;
        auto threadgroupsY = (cellsY + THREADGROUP_SIZE_Y - 1) / THREADGROUP_SIZE_Y;
        auto threadgroupsZ = (cellsZ + THREADGROUP_SIZE_Z - 1) / THREADGROUP_SIZE_Z;

        MTL::Size threadgroups(threadgroupsX, threadgroupsY, threadgroupsZ);
        MTL::Size threadsPerGroup(THREADGROUP_SIZE_X, THREADGROUP_SIZE_Y, THREADGROUP_SIZE_Z);

        // ── Pass 1: Classify cells and count vertices ────────────
        // No vertex buffer needed — only counts via atomic counter.
        {
            auto* commandBuffer = device_->_commandQueue->commandBuffer();
            if (!commandBuffer) {
                spdlog::warn("[MetalMarchingCubesPass] Failed to create command buffer for Pass 1");
                return result;
            }

            auto* encoder = commandBuffer->computeCommandEncoder();
            if (!encoder) {
                spdlog::warn("[MetalMarchingCubesPass] Failed to create compute encoder for Pass 1");
                return result;
            }

            encoder->pushDebugGroup(
                NS::String::string("MC-Classify", NS::UTF8StringEncoding));

            encoder->setComputePipelineState(classifyPipeline_);
            encoder->setTexture(hwTexture->raw(), 0);         // [[texture(0)]]
            encoder->setSamplerState(fieldSampler_, 0);        // [[sampler(0)]]
            encoder->setBuffer(uniformBuffer_, 0, 0);          // [[buffer(0)]]
            encoder->setBuffer(edgeTableBuffer_, 0, 1);        // [[buffer(1)]]
            encoder->setBuffer(triTableBuffer_, 0, 2);         // [[buffer(2)]]
            encoder->setBuffer(counterBuffer_, 0, 3);          // [[buffer(3)]]

            encoder->dispatchThreadgroups(threadgroups, threadsPerGroup);

            encoder->popDebugGroup();
            encoder->endEncoding();
            commandBuffer->commit();
            commandBuffer->waitUntilCompleted();
        }

        // Read back vertex count from atomic counter
        uint32_t totalVertices = 0;
        std::memcpy(&totalVertices, counterBuffer_->contents(), sizeof(uint32_t));

        if (totalVertices == 0) {
            spdlog::debug("[MetalMarchingCubesPass] No vertices generated (isovalue={:.4f})",
                params.isovalue);
            result.vertexCount = 0;
            result.success = true;
            return result;
        }

        spdlog::debug("[MetalMarchingCubesPass] Pass 1: {} vertices ({} triangles)",
            totalVertices, totalVertices / 3);

        // ── Allocate a fresh vertex buffer for this extraction ────
        // Each extract() call produces its own buffer — caller takes ownership.
        auto* vbuffer = allocateVertexBuffer(totalVertices);
        if (!vbuffer) return result;

        // Update maxVertices in uniforms to actual count (for safety check in shader)
        MCComputeParams pass2Params = params;
        pass2Params.maxVertices = totalVertices;
        std::memcpy(uniformBuffer_->contents(), &pass2Params, sizeof(MCComputeParams));

        // Reset counter for Pass 2 (used for atomic allocation)
        std::memcpy(counterBuffer_->contents(), &zero, sizeof(uint32_t));

        // ── Pass 2: Generate vertices ────────────────────────────
        {
            auto* commandBuffer = device_->_commandQueue->commandBuffer();
            if (!commandBuffer) {
                spdlog::warn("[MetalMarchingCubesPass] Failed to create command buffer for Pass 2");
                vbuffer->release();
                return result;
            }

            auto* encoder = commandBuffer->computeCommandEncoder();
            if (!encoder) {
                spdlog::warn("[MetalMarchingCubesPass] Failed to create compute encoder for Pass 2");
                vbuffer->release();
                return result;
            }

            encoder->pushDebugGroup(
                NS::String::string("MC-Generate", NS::UTF8StringEncoding));

            encoder->setComputePipelineState(generatePipeline_);
            encoder->setTexture(hwTexture->raw(), 0);         // [[texture(0)]]
            encoder->setSamplerState(fieldSampler_, 0);        // [[sampler(0)]]
            encoder->setBuffer(uniformBuffer_, 0, 0);          // [[buffer(0)]]
            encoder->setBuffer(edgeTableBuffer_, 0, 1);        // [[buffer(1)]]
            encoder->setBuffer(triTableBuffer_, 0, 2);         // [[buffer(2)]]
            encoder->setBuffer(counterBuffer_, 0, 3);          // [[buffer(3)]]
            encoder->setBuffer(vbuffer, 0, 4);                 // [[buffer(4)]]

            encoder->dispatchThreadgroups(threadgroups, threadsPerGroup);

            encoder->popDebugGroup();
            encoder->endEncoding();
            commandBuffer->commit();
            commandBuffer->waitUntilCompleted();
        }

        // Transfer ownership to caller — they must release or adopt this buffer.
        result.vertexBuffer = vbuffer;
        result.vertexCount = totalVertices;
        result.success = true;

        return result;
    }

    // ─── Multi-Isovalue Batch Extraction ────────────────────────────

    MCBatchResult MetalMarchingCubesPass::extractBatch(
        Texture* volumeTexture,
        const MCComputeParams& baseParams,
        const std::vector<float>& isovalues,
        const std::vector<bool>& flipNormals)
    {
        MCBatchResult batchResult;

        if (!volumeTexture || isovalues.empty()) {
            spdlog::warn("[MetalMarchingCubesPass] extractBatch: invalid args");
            return batchResult;
        }

        ensureResources();
        if (!resourcesReady_) return batchResult;

        auto* hwTexture = dynamic_cast<gpu::MetalTexture*>(volumeTexture->impl());
        if (!hwTexture || !hwTexture->raw()) {
            spdlog::warn("[MetalMarchingCubesPass] Volume texture has no Metal backing");
            return batchResult;
        }

        // Grid dimensions (shared across all layers)
        const uint32_t cellsX = baseParams.dimsX - 1;
        const uint32_t cellsY = baseParams.dimsY - 1;
        const uint32_t cellsZ = baseParams.dimsZ - 1;

        const auto threadgroupsX = (cellsX + THREADGROUP_SIZE_X - 1) / THREADGROUP_SIZE_X;
        const auto threadgroupsY = (cellsY + THREADGROUP_SIZE_Y - 1) / THREADGROUP_SIZE_Y;
        const auto threadgroupsZ = (cellsZ + THREADGROUP_SIZE_Z - 1) / THREADGROUP_SIZE_Z;

        MTL::Size threadgroups(threadgroupsX, threadgroupsY, threadgroupsZ);
        MTL::Size threadsPerGroup(THREADGROUP_SIZE_X, THREADGROUP_SIZE_Y, THREADGROUP_SIZE_Z);

        const size_t numLayers = isovalues.size();
        batchResult.layers.resize(numLayers);

        // ── Pass 1: Count vertices for all layers ────────────────
        std::vector<uint32_t> vertexCounts(numLayers, 0);

        for (size_t i = 0; i < numLayers; ++i) {
            MCComputeParams layerParams = baseParams;
            layerParams.isovalue = isovalues[i];
            if (!flipNormals.empty() && i < flipNormals.size()) {
                layerParams.flipNormals = flipNormals[i] ? 1 : 0;
            }

            std::memcpy(uniformBuffer_->contents(), &layerParams, sizeof(MCComputeParams));

            uint32_t zero = 0;
            std::memcpy(counterBuffer_->contents(), &zero, sizeof(uint32_t));

            auto* commandBuffer = device_->_commandQueue->commandBuffer();
            if (!commandBuffer) continue;
            auto* encoder = commandBuffer->computeCommandEncoder();
            if (!encoder) continue;

            encoder->setComputePipelineState(classifyPipeline_);
            encoder->setTexture(hwTexture->raw(), 0);
            encoder->setSamplerState(fieldSampler_, 0);
            encoder->setBuffer(uniformBuffer_, 0, 0);
            encoder->setBuffer(edgeTableBuffer_, 0, 1);
            encoder->setBuffer(triTableBuffer_, 0, 2);
            encoder->setBuffer(counterBuffer_, 0, 3);
            encoder->dispatchThreadgroups(threadgroups, threadsPerGroup);
            encoder->endEncoding();
            commandBuffer->commit();
            commandBuffer->waitUntilCompleted();

            std::memcpy(&vertexCounts[i], counterBuffer_->contents(), sizeof(uint32_t));
        }

        // ── Allocate per-layer vertex buffers ────────────────────
        for (size_t i = 0; i < numLayers; ++i) {
            if (vertexCounts[i] > 0) {
                batchResult.layers[i].vertexBuffer = allocateVertexBuffer(vertexCounts[i]);
            }
        }

        // ── Pass 2: Generate vertices for all layers ─────────────
        for (size_t i = 0; i < numLayers; ++i) {
            if (vertexCounts[i] == 0 || !batchResult.layers[i].vertexBuffer) {
                batchResult.layers[i].vertexCount = 0;
                continue;
            }

            MCComputeParams layerParams = baseParams;
            layerParams.isovalue = isovalues[i];
            layerParams.maxVertices = vertexCounts[i];
            if (!flipNormals.empty() && i < flipNormals.size()) {
                layerParams.flipNormals = flipNormals[i] ? 1 : 0;
            }

            std::memcpy(uniformBuffer_->contents(), &layerParams, sizeof(MCComputeParams));

            uint32_t zero = 0;
            std::memcpy(counterBuffer_->contents(), &zero, sizeof(uint32_t));

            auto* commandBuffer = device_->_commandQueue->commandBuffer();
            if (!commandBuffer) continue;
            auto* encoder = commandBuffer->computeCommandEncoder();
            if (!encoder) continue;

            encoder->setComputePipelineState(generatePipeline_);
            encoder->setTexture(hwTexture->raw(), 0);
            encoder->setSamplerState(fieldSampler_, 0);
            encoder->setBuffer(uniformBuffer_, 0, 0);
            encoder->setBuffer(edgeTableBuffer_, 0, 1);
            encoder->setBuffer(triTableBuffer_, 0, 2);
            encoder->setBuffer(counterBuffer_, 0, 3);
            encoder->setBuffer(batchResult.layers[i].vertexBuffer, 0, 4);
            encoder->dispatchThreadgroups(threadgroups, threadsPerGroup);
            encoder->endEncoding();
            commandBuffer->commit();
            commandBuffer->waitUntilCompleted();

            batchResult.layers[i].vertexCount = vertexCounts[i];
        }

        batchResult.success = true;
        return batchResult;
    }

} // namespace visutwin::canvas
