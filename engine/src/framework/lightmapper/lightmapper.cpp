// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#include "lightmapper.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <thread>

#include <spdlog/spdlog.h>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/mesh.h"
#include "scene/materials/standardMaterial.h"

namespace visutwin::canvas
{
    namespace
    {
        // PackedVertex byte offsets (mirrors the parser's 56-byte layout; larger
        // strides — e.g. skinned 88 — share this 56-byte prefix).
        constexpr size_t OFF_POS = 0;    // float3
        constexpr size_t OFF_NRM = 12;   // float3
        constexpr size_t OFF_UV1 = 48;   // float2

        struct Vertex { Vector3 pos; Vector3 nrm; float u1; float v1; };

        float readF(const uint8_t* base, const size_t off)
        {
            float v;
            std::memcpy(&v, base + off, sizeof(float));
            return v;
        }

        // Read indexed triangles from a mesh's CPU-side storage.
        template <typename Emit>
        void forEachTriangle(const Mesh& mesh, const Emit& emit)
        {
            const auto vb = mesh.getVertexBuffer();
            const auto ib = mesh.getIndexBuffer(0);
            if (!vb || vb->storage().empty() || !vb->format()) {
                return;
            }
            const size_t stride = static_cast<size_t>(vb->format()->size());
            const uint8_t* vdata = vb->storage().data();
            const int numVerts = vb->numVertices();

            const auto vtx = [&](const uint32_t i) -> Vertex {
                const uint8_t* p = vdata + static_cast<size_t>(i) * stride;
                return {Vector3(readF(p, OFF_POS), readF(p, OFF_POS + 4), readF(p, OFF_POS + 8)),
                        Vector3(readF(p, OFF_NRM), readF(p, OFF_NRM + 4), readF(p, OFF_NRM + 8)),
                        readF(p, OFF_UV1), readF(p, OFF_UV1 + 4)};
            };

            if (ib && !ib->storage().empty()) {
                const uint8_t* idx = ib->storage().data();
                const size_t elem = (ib->format() == INDEXFORMAT_UINT32) ? 4
                                   : (ib->format() == INDEXFORMAT_UINT16) ? 2 : 1;
                const size_t idxCount = ib->storage().size() / elem;
                const auto index = [&](const size_t k) -> uint32_t {
                    switch (ib->format()) {
                        case INDEXFORMAT_UINT32: { uint32_t v; std::memcpy(&v, idx + k * 4, 4); return v; }
                        case INDEXFORMAT_UINT16: { uint16_t v; std::memcpy(&v, idx + k * 2, 2); return v; }
                        default: return idx[k];
                    }
                };
                for (size_t k = 0; k + 2 < idxCount; k += 3) {
                    emit(vtx(index(k)), vtx(index(k + 1)), vtx(index(k + 2)));
                }
            } else {
                for (int k = 0; k + 2 < numVerts; k += 3) {
                    emit(vtx(static_cast<uint32_t>(k)), vtx(static_cast<uint32_t>(k + 1)),
                         vtx(static_cast<uint32_t>(k + 2)));
                }
            }
        }

        // Möller-Trumbore ray/triangle. Returns hit distance in `t` (> 0).
        bool rayTri(const Vector3& o, const Vector3& d, const Vector3& a,
            const Vector3& b, const Vector3& c, float& t)
        {
            const Vector3 e1 = b - a, e2 = c - a;
            const Vector3 pv = d.cross(e2);
            const float det = e1.dot(pv);
            if (std::fabs(det) < 1e-8f) return false;
            const float inv = 1.0f / det;
            const Vector3 tv = o - a;
            const float u = tv.dot(pv) * inv;
            if (u < 0.0f || u > 1.0f) return false;
            const Vector3 qv = tv.cross(e1);
            const float v = d.dot(qv) * inv;
            if (v < 0.0f || u + v > 1.0f) return false;
            t = e2.dot(qv) * inv;
            return t > 1e-4f;
        }

        // van der Corput radical inverse (base 2) for low-discrepancy AO.
        float radicalInverse2(uint32_t bits)
        {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<float>(bits) * 2.3283064365386963e-10f;
        }

        /// Lightmaps store LINEAR light — the shader samples them without a decode, so the
        /// GPU baker's accumulating virtual-light passes can sum in linear space. 8 bits of
        /// linear is coarse in the darks, but a lightmap is low-frequency by nature.
        uint8_t toLightmapByte(const float linear)
        {
            const float c = std::clamp(linear, 0.0f, 1.0f);
            return static_cast<uint8_t>(c * 255.0f + 0.5f);
        }

        // Median-split BVH over world-space triangles for fast any-hit ray tests.
        // Without it, brute-force ray casting against a tessellated occluder makes
        // a per-texel AO bake O(texels · rays · triangles) — minutes, not seconds.
        constexpr float GOLDEN_ANGLE = 2.399963229728653f;  // upstream _goldenAngle

        /// Upstream random.circlePointDeterministic — evenly spread points in a unit disc.
        void circlePointDeterministic(float& x, float& y, const int index, const int numPoints)
        {
            const float theta = static_cast<float>(index) * GOLDEN_ANGLE;
            const float r = std::sqrt(static_cast<float>(index) / static_cast<float>(std::max(numPoints, 1)));
            x = r * std::cos(theta);
            y = r * std::sin(theta);
        }

        /// Upstream random.spherePointDeterministic — Fibonacci sphere, optionally
        /// covering only the top `end` part of the sphere (y from +1 downwards).
        Vector3 spherePointDeterministic(const int index, const int numPoints, const float end)
        {
            const float start = 1.0f;                 // upstream: 1 - 2 * 0
            const float finish = 1.0f - 2.0f * end;
            const float t = static_cast<float>(index) / static_cast<float>(std::max(numPoints, 1));
            const float y = start + (finish - start) * t;
            const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
            const float theta = GOLDEN_ANGLE * static_cast<float>(index);
            return Vector3(std::cos(theta) * radius, y, std::sin(theta) * radius);
        }

        int nextPowerOfTwo(int value)
        {
            int result = 1;
            while (result < value) {
                result <<= 1;
            }
            return result;
        }

        struct TriData { Vector3 a, b, c; };
        struct BvhNode { Vector3 bmin, bmax; int start, count, left; };

        class Bvh
        {
        public:
            void build(const std::vector<TriData>& tris)
            {
                _tris = &tris;
                _order.resize(tris.size());
                for (size_t i = 0; i < tris.size(); ++i) _order[i] = static_cast<int>(i);
                _nodes.clear();
                if (!tris.empty()) {
                    _nodes.reserve(tris.size() * 2);
                    buildNode(0, static_cast<int>(tris.size()));
                }
            }

            // Returns true if any triangle is hit within [eps, maxDist).
            bool anyHit(const Vector3& o, const Vector3& d, const float maxDist) const
            {
                if (_nodes.empty()) return false;
                const Vector3 inv(1.0f / d.getX(), 1.0f / d.getY(), 1.0f / d.getZ());
                int stack[64];
                int sp = 0;
                stack[sp++] = 0;
                while (sp > 0) {
                    const BvhNode& n = _nodes[static_cast<size_t>(stack[--sp])];
                    if (!slab(o, inv, n.bmin, n.bmax, maxDist)) continue;
                    if (n.count > 0) {
                        for (int i = 0; i < n.count; ++i) {
                            const TriData& t = (*_tris)[static_cast<size_t>(_order[static_cast<size_t>(n.start + i)])];
                            float hit;
                            if (rayTri(o, d, t.a, t.b, t.c, hit) && hit < maxDist) return true;
                        }
                    } else {
                        stack[sp++] = n.left;
                        stack[sp++] = n.left + 1;
                    }
                }
                return false;
            }

        private:
            static bool slab(const Vector3& o, const Vector3& inv, const Vector3& bmin,
                const Vector3& bmax, const float maxDist)
            {
                float t0 = 0.0f, t1 = maxDist;
                for (int a = 0; a < 3; ++a) {
                    const float oi = a == 0 ? o.getX() : (a == 1 ? o.getY() : o.getZ());
                    const float ii = a == 0 ? inv.getX() : (a == 1 ? inv.getY() : inv.getZ());
                    const float lo = a == 0 ? bmin.getX() : (a == 1 ? bmin.getY() : bmin.getZ());
                    const float hi = a == 0 ? bmax.getX() : (a == 1 ? bmax.getY() : bmax.getZ());
                    float near = (lo - oi) * ii, far = (hi - oi) * ii;
                    if (near > far) std::swap(near, far);
                    t0 = std::max(t0, near);
                    t1 = std::min(t1, far);
                    if (t0 > t1) return false;
                }
                return true;
            }

            int buildNode(const int start, const int count)
            {
                const int nodeIdx = static_cast<int>(_nodes.size());
                _nodes.push_back({});
                Vector3 bmin(1e30f, 1e30f, 1e30f), bmax(-1e30f, -1e30f, -1e30f);
                Vector3 cmin(1e30f, 1e30f, 1e30f), cmax(-1e30f, -1e30f, -1e30f);
                for (int i = 0; i < count; ++i) {
                    const TriData& t = (*_tris)[static_cast<size_t>(_order[static_cast<size_t>(start + i)])];
                    const Vector3 lo(std::min({t.a.getX(), t.b.getX(), t.c.getX()}),
                                     std::min({t.a.getY(), t.b.getY(), t.c.getY()}),
                                     std::min({t.a.getZ(), t.b.getZ(), t.c.getZ()}));
                    const Vector3 hi(std::max({t.a.getX(), t.b.getX(), t.c.getX()}),
                                     std::max({t.a.getY(), t.b.getY(), t.c.getY()}),
                                     std::max({t.a.getZ(), t.b.getZ(), t.c.getZ()}));
                    bmin = vmin(bmin, lo); bmax = vmax(bmax, hi);
                    const Vector3 ctr = (lo + hi) * 0.5f;
                    cmin = vmin(cmin, ctr); cmax = vmax(cmax, ctr);
                }

                if (count <= 4) {
                    _nodes[static_cast<size_t>(nodeIdx)] = {bmin, bmax, start, count, -1};
                    return nodeIdx;
                }

                const Vector3 ext = cmax - cmin;
                const int axis = ext.getX() > ext.getY() ? (ext.getX() > ext.getZ() ? 0 : 2)
                                                         : (ext.getY() > ext.getZ() ? 1 : 2);
                const float mid = 0.5f * (axisVal(cmin, axis) + axisVal(cmax, axis));
                int* first = _order.data() + start;
                int* last = first + count;
                int* split = std::partition(first, last, [&](const int idx) {
                    const TriData& t = (*_tris)[static_cast<size_t>(idx)];
                    const Vector3 ctr = (t.a + t.b + t.c) * (1.0f / 3.0f);
                    return axisVal(ctr, axis) < mid;
                });
                int leftCount = static_cast<int>(split - first);
                if (leftCount == 0 || leftCount == count) leftCount = count / 2;   // degenerate guard

                const int leftChild = buildNode(start, leftCount);
                buildNode(start + leftCount, count - leftCount);
                _nodes[static_cast<size_t>(nodeIdx)] = {bmin, bmax, 0, 0, leftChild};
                return nodeIdx;
            }

            static float axisVal(const Vector3& v, const int a) {
                return a == 0 ? v.getX() : (a == 1 ? v.getY() : v.getZ());
            }
            static Vector3 vmin(const Vector3& a, const Vector3& b) {
                return Vector3(std::min(a.getX(), b.getX()), std::min(a.getY(), b.getY()), std::min(a.getZ(), b.getZ()));
            }
            static Vector3 vmax(const Vector3& a, const Vector3& b) {
                return Vector3(std::max(a.getX(), b.getX()), std::max(a.getY(), b.getY()), std::max(a.getZ(), b.getZ()));
            }

            const std::vector<TriData>* _tris = nullptr;
            std::vector<int> _order;
            std::vector<BvhNode> _nodes;
        };
    }

    Lightmapper::Lightmapper(GraphicsDevice* device) : _device(device) {}

    void Lightmapper::clear()
    {
        _lights.clear();
        _occluders.clear();
    }

    void Lightmapper::addOccluder(const Mesh& mesh, const Matrix4& worldTransform)
    {
        forEachTriangle(mesh, [&](const Vertex& v0, const Vertex& v1, const Vertex& v2) {
            _occluders.push_back({worldTransform.transformPoint(v0.pos),
                                  worldTransform.transformPoint(v1.pos),
                                  worldTransform.transformPoint(v2.pos)});
        });
    }

    std::shared_ptr<Texture> Lightmapper::bake(const Mesh& target, const Matrix4& worldTransform,
        const Options& options)
    {
        if (!_device) return nullptr;

        // Resolution: either fixed, or derived from the target's world-space bounds
        // the way upstream's calculateLightmapSize does.
        int size = std::clamp(options.lightmapSize, 8, 4096);
        if (options.sizeMultiplier > 0.0f) {
            Vector3 bmin(1e30f, 1e30f, 1e30f);
            Vector3 bmax(-1e30f, -1e30f, -1e30f);
            forEachTriangle(target, [&](const Vertex& a, const Vertex& b, const Vertex& c) {
                for (const auto& v : {a, b, c}) {
                    const Vector3 w = worldTransform.transformPoint(v.pos);
                    bmin = Vector3(std::min(bmin.getX(), w.getX()), std::min(bmin.getY(), w.getY()),
                                   std::min(bmin.getZ(), w.getZ()));
                    bmax = Vector3(std::max(bmax.getX(), w.getX()), std::max(bmax.getY(), w.getY()),
                                   std::max(bmax.getZ(), w.getZ()));
                }
            });
            if (bmax.getX() >= bmin.getX()) {
                // upstream uses the half extents and the three face areas, unit area per axis
                const float hx = (bmax.getX() - bmin.getX()) * 0.5f;
                const float hy = (bmax.getY() - bmin.getY()) * 0.5f;
                const float hz = (bmax.getZ() - bmin.getZ()) * 0.5f;
                const float totalArea = std::sqrt(hy * hz + hx * hz + hx * hy);
                size = std::clamp(nextPowerOfTwo(static_cast<int>(totalArea * options.sizeMultiplier)),
                    8, std::clamp(options.maxResolution, 8, 4096));
            }
        }

        // BVH over occluder triangles for fast shadow/AO any-hit queries.
        std::vector<TriData> triData;
        triData.reserve(_occluders.size());
        for (const auto& t : _occluders) triData.push_back({t.a, t.b, t.c});
        Bvh bvh;
        bvh.build(triData);
        const auto occluded = [&](const Vector3& origin, const Vector3& dir, const float maxDist) {
            return bvh.anyHit(origin, dir, maxDist);
        };

        // Direct + ambient/AO lighting at a world surface point.
        const auto shade = [&](const Vector3& P, const Vector3& N) -> Color {
            const Vector3 origin = P + N * 1e-3f;
            Color lit(0.0f, 0.0f, 0.0f, 1.0f);

            for (const auto& light : _lights) {
                Vector3 L;
                float atten = 1.0f;
                if (light.type == LightType::LIGHTTYPE_DIRECTIONAL) {
                    L = (light.direction * -1.0f).normalized();
                } else {
                    Vector3 toLight = light.position - P;
                    const float dist = toLight.length();
                    if (dist < 1e-5f) continue;
                    L = toLight * (1.0f / dist);
                    if (light.range > 0.0f) {
                        const float f = std::clamp(1.0f - dist / light.range, 0.0f, 1.0f);
                        atten = f * f;
                    }
                    if (light.type == LightType::LIGHTTYPE_SPOT) {
                        const float cd = (L * -1.0f).dot(light.direction.normalized());
                        const float spot = std::clamp((cd - light.outerConeCos) /
                            std::max(light.innerConeCos - light.outerConeCos, 1e-4f), 0.0f, 1.0f);
                        atten *= spot * spot;
                    }
                }
                const float ndl = std::max(N.dot(L), 0.0f);
                if (ndl <= 0.0f || atten <= 0.0f) continue;

                const float reach = (light.type == LightType::LIGHTTYPE_DIRECTIONAL)
                    ? 1e6f : (light.position - P).length();

                float visibility = 1.0f;
                if (light.castShadows) {
                    const bool soft = light.type == LightType::LIGHTTYPE_DIRECTIONAL &&
                        light.bakeNumSamples > 1 && light.bakeArea > 0.0f;
                    if (!soft) {
                        if (occluded(origin, L, reach)) continue;
                    } else {
                        // Spread the shadow ray over a bakeArea-degree cone, the same
                        // spread upstream applies by rotating its virtual lights.
                        const Vector3 up = std::fabs(L.getY()) < 0.99f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
                        const Vector3 tangent = up.cross(L).normalized();
                        const Vector3 bitangent = L.cross(tangent);
                        const float spread = std::tan(light.bakeArea * 0.5f *
                            std::numbers::pi_v<float> / 180.0f);
                        int unshadowed = 0;
                        for (int vs = 0; vs < light.bakeNumSamples; ++vs) {
                            float jx = 0.0f, jy = 0.0f;
                            if (vs > 0) {
                                circlePointDeterministic(jx, jy, vs, light.bakeNumSamples);
                            }
                            const Vector3 dir = (L + tangent * (jx * spread) +
                                bitangent * (jy * spread)).normalized();
                            if (!occluded(origin, dir, reach)) ++unshadowed;
                        }
                        visibility = static_cast<float>(unshadowed) /
                            static_cast<float>(light.bakeNumSamples);
                        if (visibility <= 0.0f) continue;
                    }
                }

                const float s = ndl * atten * light.intensity * visibility;
                lit.r += light.color.r * s;
                lit.g += light.color.g * s;
                lit.b += light.color.b * s;
            }

            if (options.ambientBake && options.ambientBakeNumSamples > 0) {
                // Upstream bakes ambient as N virtual directional lights spread over the
                // top `spherePart` of the sphere. Here the same distribution drives N
                // occlusion rays, weighted by N·L like the virtual lights' own N·L term.
                float weight = 0.0f;
                float visible = 0.0f;
                for (int as = 0; as < options.ambientBakeNumSamples; ++as) {
                    const Vector3 dir = spherePointDeterministic(as, options.ambientBakeNumSamples,
                        std::clamp(options.ambientBakeSpherePart, 0.01f, 1.0f));
                    const float ndl = N.dot(dir);
                    if (ndl <= 0.0f) continue;
                    weight += ndl;
                    if (!occluded(origin, dir, options.aoRadius)) {
                        visible += ndl;
                    }
                }
                float ambientOcclusion = weight > 0.0f ? visible / weight : 1.0f;

                // upstream bakeLmEnd: contrast around 0.5, then brightness, then saturate
                ambientOcclusion = ((ambientOcclusion - 0.5f) *
                    std::max(options.ambientBakeOcclusionContrast + 1.0f, 0.0f)) + 0.5f;
                ambientOcclusion = std::clamp(ambientOcclusion + options.ambientBakeOcclusionBrightness,
                    0.0f, 1.0f);

                lit.r += (options.ambient.r + options.skyColor.r) * ambientOcclusion;
                lit.g += (options.ambient.g + options.skyColor.g) * ambientOcclusion;
                lit.b += (options.ambient.b + options.skyColor.b) * ambientOcclusion;
                return lit;
            }

            float ao = 1.0f;
            if (options.ambientOcclusion && options.aoSamples > 0) {
                // cosine-weighted hemisphere via a low-discrepancy sequence.
                const Vector3 up = std::fabs(N.getY()) < 0.99f ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
                const Vector3 tangent = up.cross(N).normalized();
                const Vector3 bitangent = N.cross(tangent);
                int unoccluded = 0;
                for (int s = 0; s < options.aoSamples; ++s) {
                    const float u1 = (static_cast<float>(s) + 0.5f) / static_cast<float>(options.aoSamples);
                    const float u2 = radicalInverse2(static_cast<uint32_t>(s) + 1u);
                    const float r = std::sqrt(u1);
                    const float phi = 2.0f * std::numbers::pi_v<float> * u2;
                    const float x = r * std::cos(phi);
                    const float y = r * std::sin(phi);
                    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));
                    const Vector3 dir = (tangent * x + bitangent * y + N * z).normalized();
                    if (!occluded(origin, dir, options.aoRadius)) ++unoccluded;
                }
                ao = static_cast<float>(unoccluded) / static_cast<float>(options.aoSamples);
            }
            lit.r += (options.ambient.r + options.skyColor.r) * ao;
            lit.g += (options.ambient.g + options.skyColor.g) * ao;
            lit.b += (options.ambient.b + options.skyColor.b) * ao;
            return lit;
        };

        std::vector<Vector3> accum(static_cast<size_t>(size) * size, Vector3(0, 0, 0));
        std::vector<uint8_t> covered(static_cast<size_t>(size) * size, 0);

        // Phase 1 (cheap): rasterize target triangles in UV1 space, recording the
        // world surface point + normal for each covered texel.
        std::vector<Vector3> posBuf(static_cast<size_t>(size) * size);
        std::vector<Vector3> nrmBuf(static_cast<size_t>(size) * size);
        std::vector<uint32_t> work;   // covered texel indices to shade

        forEachTriangle(target, [&](const Vertex& a, const Vertex& b, const Vertex& c) {
            const Vector3 wp[3] = {worldTransform.transformPoint(a.pos),
                                   worldTransform.transformPoint(b.pos),
                                   worldTransform.transformPoint(c.pos)};
            const Vector3 wn[3] = {a.nrm.transformNormal(worldTransform).normalized(),
                                   b.nrm.transformNormal(worldTransform).normalized(),
                                   c.nrm.transformNormal(worldTransform).normalized()};
            const float fs = static_cast<float>(size);
            const float px[3] = {a.u1 * fs, b.u1 * fs, c.u1 * fs};
            const float py[3] = {a.v1 * fs, b.v1 * fs, c.v1 * fs};

            const int minX = std::max(0, static_cast<int>(std::floor(std::min({px[0], px[1], px[2]}))) - 1);
            const int maxX = std::min(size - 1, static_cast<int>(std::ceil(std::max({px[0], px[1], px[2]}))) + 1);
            const int minY = std::max(0, static_cast<int>(std::floor(std::min({py[0], py[1], py[2]}))) - 1);
            const int maxY = std::min(size - 1, static_cast<int>(std::ceil(std::max({py[0], py[1], py[2]}))) + 1);

            const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
            if (std::fabs(area) < 1e-8f) return;
            const float invArea = 1.0f / area;

            for (int y = minY; y <= maxY; ++y) {
                for (int x = minX; x <= maxX; ++x) {
                    const float fx = static_cast<float>(x) + 0.5f;
                    const float fy = static_cast<float>(y) + 0.5f;
                    // barycentric of the pixel center in the UV-space triangle
                    const float w0 = ((px[1] - fx) * (py[2] - fy) - (px[2] - fx) * (py[1] - fy)) * invArea;
                    const float w1 = ((px[2] - fx) * (py[0] - fy) - (px[0] - fx) * (py[2] - fy)) * invArea;
                    const float w2 = 1.0f - w0 - w1;
                    constexpr float bias = -0.01f;   // slight conservative expansion
                    if (w0 < bias || w1 < bias || w2 < bias) continue;

                    const size_t idx = static_cast<size_t>(y) * size + x;
                    if (covered[idx]) continue;
                    posBuf[idx] = wp[0] * w0 + wp[1] * w1 + wp[2] * w2;
                    nrmBuf[idx] = (wn[0] * w0 + wn[1] * w1 + wn[2] * w2).normalized();
                    covered[idx] = 1;
                    work.push_back(static_cast<uint32_t>(idx));
                }
            }
        });

        // Phase 2 (expensive): shade covered texels in parallel (ray casting).
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        const size_t threadCount = std::min<size_t>(hw, std::max<size_t>(1, work.size() / 1024 + 1));
        const auto shadeRange = [&](const size_t begin, const size_t end) {
            for (size_t w = begin; w < end; ++w) {
                const uint32_t idx = work[w];
                const Color lit = shade(posBuf[idx], nrmBuf[idx]);
                accum[idx] = Vector3(lit.r, lit.g, lit.b);
            }
        };
        if (threadCount <= 1 || work.empty()) {
            shadeRange(0, work.size());
        } else {
            std::vector<std::thread> pool;
            const size_t chunk = (work.size() + threadCount - 1) / threadCount;
            for (size_t t = 0; t < threadCount; ++t) {
                const size_t b = t * chunk;
                const size_t e = std::min(work.size(), b + chunk);
                if (b < e) pool.emplace_back(shadeRange, b, e);
            }
            for (auto& th : pool) th.join();
        }

        // Bilateral denoise over the shaded texels (upstream's bilateralDeNoise pass,
        // driven by the same two sigmas: filterRange spatially, filterSmoothness on
        // intensity, so lighting detail survives while ray noise is smoothed away).
        // Runs before dilation so only real, shaded texels contribute.
        if (options.filterEnabled && !work.empty()) {
            const float sigmaSpace = std::max(options.filterRange, 0.01f);
            const float sigmaValue = std::max(options.filterSmoothness, 0.01f);
            const int radius = std::clamp(static_cast<int>(std::ceil(sigmaSpace)), 1, 7);
            const float invTwoSigmaSpaceSq = 1.0f / (2.0f * sigmaSpace * sigmaSpace);
            const float invTwoSigmaValueSq = 1.0f / (2.0f * sigmaValue * sigmaValue);

            std::vector<Vector3> filtered = accum;
            const auto filterRange = [&](const size_t begin, const size_t end) {
                for (size_t w = begin; w < end; ++w) {
                    const uint32_t idx = work[w];
                    const int cx = static_cast<int>(idx % static_cast<size_t>(size));
                    const int cy = static_cast<int>(idx / static_cast<size_t>(size));
                    const Vector3 center = accum[idx];

                    Vector3 sum(0, 0, 0);
                    float weightSum = 0.0f;
                    for (int dy = -radius; dy <= radius; ++dy) {
                        const int y = cy + dy;
                        if (y < 0 || y >= size) continue;
                        for (int dx = -radius; dx <= radius; ++dx) {
                            const int x = cx + dx;
                            if (x < 0 || x >= size) continue;
                            const size_t nIdx = static_cast<size_t>(y) * size + x;
                            if (!covered[nIdx]) continue;

                            const Vector3 sample = accum[nIdx];
                            const Vector3 delta = sample - center;
                            const float spatial = static_cast<float>(dx * dx + dy * dy) * invTwoSigmaSpaceSq;
                            const float range = delta.lengthSquared() * invTwoSigmaValueSq;
                            const float weight = std::exp(-(spatial + range));
                            sum = sum + sample * weight;
                            weightSum += weight;
                        }
                    }
                    if (weightSum > 0.0f) {
                        filtered[idx] = sum * (1.0f / weightSum);
                    }
                }
            };
            if (threadCount <= 1) {
                filterRange(0, work.size());
            } else {
                std::vector<std::thread> pool;
                const size_t chunk = (work.size() + threadCount - 1) / threadCount;
                for (size_t t = 0; t < threadCount; ++t) {
                    const size_t b = t * chunk;
                    const size_t e = std::min(work.size(), b + chunk);
                    if (b < e) pool.emplace_back(filterRange, b, e);
                }
                for (auto& th : pool) th.join();
            }
            accum.swap(filtered);
        }

        // Dilate covered texels outward to fill seams (bilinear sampling at UV
        // borders otherwise fetches black).
        for (int iter = 0; iter < options.dilatePixels; ++iter) {
            std::vector<Vector3> next = accum;
            std::vector<uint8_t> nextCov = covered;
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const size_t idx = static_cast<size_t>(y) * size + x;
                    if (covered[idx]) continue;
                    Vector3 sum(0, 0, 0);
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
                            const size_t nidx = static_cast<size_t>(ny) * size + nx;
                            if (covered[nidx]) { sum = sum + accum[nidx]; ++n; }
                        }
                    }
                    if (n > 0) { next[idx] = sum * (1.0f / static_cast<float>(n)); nextCov[idx] = 1; }
                }
            }
            accum.swap(next);
            covered.swap(nextCov);
        }

        // Encode sRGB RGBA8 (the shader pow(2.2)-decodes the lightmap).
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
        for (size_t i = 0; i < accum.size(); ++i) {
            pixels[i * 4 + 0] = toLightmapByte(accum[i].getX());
            pixels[i * 4 + 1] = toLightmapByte(accum[i].getY());
            pixels[i * 4 + 2] = toLightmapByte(accum[i].getZ());
            pixels[i * 4 + 3] = 255;
        }

        TextureOptions texOptions;
        texOptions.name = "lightmap";
        texOptions.width = static_cast<uint32_t>(size);
        texOptions.height = static_cast<uint32_t>(size);
        texOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        texOptions.mipmaps = false;
        texOptions.minFilter = FilterMode::FILTER_LINEAR;
        texOptions.magFilter = FilterMode::FILTER_LINEAR;
        auto texture = std::make_shared<Texture>(_device, texOptions);
        texture->setLevelData(0, pixels.data(), pixels.size());
        texture->upload();

        spdlog::info("Lightmapper: baked {}x{} lightmap ({} lights, {} occluder tris, AO {})",
            size, size, _lights.size(), _occluders.size(), options.ambientOcclusion);
        return texture;
    }

    void Lightmapper::bakeAndApply(StandardMaterial* material, const Mesh& target,
        const Matrix4& worldTransform, const Options& options)
    {
        auto tex = bake(target, worldTransform, options);
        if (material && tex) {
            material->setLightMap(tex.get());
        }
    }
}
