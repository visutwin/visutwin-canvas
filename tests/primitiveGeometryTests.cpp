// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Texture orientation of the built-in primitives. A texture's row 0 is the TOP of
// the image on both backends (loaded images are stored unflipped, render targets
// have a top origin), so v = 0 must sit where the top of the picture belongs.
// Upstream writes `(u, 1 - v)` for every primitive and gets that; the plane here
// wrote plain `v`, so every image on a plane — and render-to-texture's tv — came
// out upside down. A checkerboard cannot show this, and neither can a render of
// the plane lying flat unless the image is asymmetric, so the convention is held
// here instead.
//
// Also pinned, for EVERY primitive: the tangent points along +u, and the bitangent,
// cross(n, t) * w as the forward shaders build it, toward decreasing v — the image's
// top row, which is where a normal map's green channel points. That is upstream's
// derivative TBN (it negates the dP/dv axis), the frame every upstream primitive is
// actually shaded with. Before these frames were derived from the UVs, the box wrote
// (1, 0, 0) on every face (parallel to the normal on +/-X) and the sphere and capsule
// caps had both tangent and bitangent reversed.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "framework/components/render/primitiveGeometry.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    bool near(const float a, const float b, const float eps = 1e-4f)
    {
        return std::fabs(a - b) <= eps;
    }

    struct Vertex
    {
        float px, py, pz, nx, ny, nz, tx, ty, tz, tw, u, v;
    };

    Vertex vertexAt(const PrimitiveGeometry& g, const size_t i)
    {
        return {
            g.positions[i * 3], g.positions[i * 3 + 1], g.positions[i * 3 + 2],
            g.normals[i * 3], g.normals[i * 3 + 1], g.normals[i * 3 + 2],
            g.tangents[i * 4], g.tangents[i * 4 + 1], g.tangents[i * 4 + 2], g.tangents[i * 4 + 3],
            g.uvs[i * 2], g.uvs[i * 2 + 1]
        };
    }

    size_t vertexCount(const PrimitiveGeometry& g)
    {
        return g.positions.size() / 3;
    }

    // v at the highest (+Y) and lowest (-Y) vertices of a side surface selected by
    // `select`; an upright image puts its top row, v = 0, at the top.
    template <typename Select>
    void checkImageTopIsUp(const PrimitiveGeometry& g, Select select, const char* topWhat, const char* bottomWhat)
    {
        float top = -1e9f, bottom = 1e9f, vTop = -1.0f, vBottom = -1.0f;
        for (size_t i = 0; i < vertexCount(g); ++i) {
            const Vertex vx = vertexAt(g, i);
            if (!select(vx)) {
                continue;
            }
            if (vx.py > top) { top = vx.py; vTop = vx.v; }
            if (vx.py < bottom) { bottom = vx.py; vBottom = vx.v; }
        }
        check(near(vTop, 0.0f), topWhat);
        check(near(vBottom, 1.0f), bottomWhat);
    }
}

int main()
{
    std::cout << "primitive geometry: image top row at the top of each primitive\n";

    // PLANE. Upstream: z = +0.5 carries v = 1, z = -0.5 carries v = 0, u runs +X.
    {
        const PrimitiveGeometry plane = createPlaneGeometry();
        bool farTop = true, nearBottom = true, uAlongX = true, frame = true;
        for (size_t i = 0; i < vertexCount(plane); ++i) {
            const Vertex vx = vertexAt(plane, i);
            if (near(vx.pz, -0.5f) && !near(vx.v, 0.0f)) farTop = false;
            if (near(vx.pz, 0.5f) && !near(vx.v, 1.0f)) nearBottom = false;
            if (!near(vx.u, vx.px + 0.5f)) uAlongX = false;

            // Tangent along +u, i.e. +X.
            if (!(vx.tx > 0.99f)) frame = false;
            // Bitangent = cross(n, t) * w must point along +v, i.e. -Z.
            const float bx = (vx.ny * vx.tz - vx.nz * vx.ty) * vx.tw;
            const float bz = (vx.nx * vx.ty - vx.ny * vx.tx) * vx.tw;
            if (!(bz < -0.99f && near(bx, 0.0f))) frame = false;
        }
        check(vertexCount(plane) > 0, "plane has vertices");
        check(farTop, "plane: the z = -0.5 edge samples the image's TOP row (v = 0)");
        check(nearBottom, "plane: the z = +0.5 edge samples the image's BOTTOM row (v = 1)");
        check(uAlongX, "plane: u runs from 0 at x = -0.5 to 1 at x = +0.5");
        check(frame, "plane: tangent is +u and cross(n, t) * w is +v");

        // The render-to-texture tv: a plane stood up with a +90 degree X rotation maps
        // local (x, 0, z) to world (x, -z, 0), so the v = 0 edge must land at +Y.
        float vAtWorldTop = -1.0f;
        for (size_t i = 0; i < vertexCount(plane); ++i) {
            const Vertex vx = vertexAt(plane, i);
            if (near(-vx.pz, 0.5f)) vAtWorldTop = vx.v;
        }
        check(near(vAtWorldTop, 0.0f), "plane rotated +90 about X shows the image upright");
    }

    // BOX front face (+Z normal): top row at +Y, u from -X to +X.
    {
        const PrimitiveGeometry box = createBoxGeometry();
        auto front = [](const Vertex& vx) { return vx.nz > 0.99f; };
        checkImageTopIsUp(box, front, "box front: top edge samples v = 0", "box front: bottom edge samples v = 1");
        bool uAlongX = true;
        for (size_t i = 0; i < vertexCount(box); ++i) {
            const Vertex vx = vertexAt(box, i);
            if (front(vx) && !near(vx.u, vx.px + 0.5f)) uAlongX = false;
        }
        check(uAlongX, "box front: u runs from 0 at x = -0.5 to 1 at x = +0.5");
    }

    // SPHERE: north pole v = 0.
    checkImageTopIsUp(createSphereGeometry(), [](const Vertex&) { return true; },
        "sphere: north pole samples v = 0", "sphere: south pole samples v = 1");

    // CYLINDER and CONE bodies (normals not vertical): top ring v = 0.
    auto body = [](const Vertex& vx) { return std::fabs(vx.ny) < 0.99f; };
    checkImageTopIsUp(createCylinderGeometry(), body,
        "cylinder body: top ring samples v = 0", "cylinder body: bottom ring samples v = 1");
    checkImageTopIsUp(createConeGeometry(), body,
        "cone body: apex samples v = 0", "cone body: base ring samples v = 1");

    // TANGENT FRAMES, every primitive. The forward shaders build the bitangent as
    // cross(n, t) * w, so for a normal map to light the same way on every face the
    // tangent has to follow +u and that bitangent +v. The reference is each
    // triangle's own UV gradient (dP/du, dP/dv — the quantity upstream's
    // calculateTangents accumulates), projected into the vertex's tangent plane.
    std::cout << "primitive geometry: tangent frames follow the UVs\n";
    struct Named { const char* name; PrimitiveGeometry geometry; };
    const Named primitives[] = {
        {"plane", createPlaneGeometry()},
        {"box", createBoxGeometry()},
        {"sphere", createSphereGeometry()},
        {"cylinder", createCylinderGeometry()},
        {"cone", createConeGeometry()},
        {"capsule", createCapsuleGeometry()},
    };
    for (const auto& [name, g] : primitives) {
        size_t tested = 0;
        size_t badLength = 0, badOrtho = 0, badU = 0, badV = 0;
        float worstU = 1.0f, worstV = 1.0f, worstOrtho = 0.0f;
        for (size_t tri = 0; tri + 2 < g.indices.size(); tri += 3) {
            const uint32_t idx[3] = {g.indices[tri], g.indices[tri + 1], g.indices[tri + 2]};
            const Vertex a = vertexAt(g, idx[0]), b = vertexAt(g, idx[1]), c = vertexAt(g, idx[2]);
            const float e1[3] = {b.px - a.px, b.py - a.py, b.pz - a.pz};
            const float e2[3] = {c.px - a.px, c.py - a.py, c.pz - a.pz};
            const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            if (cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2] < 1e-12f) {
                continue; // two corners coincide (a pole or an apex)
            }
            const float s1 = b.u - a.u, s2 = c.u - a.u, t1 = b.v - a.v, t2 = c.v - a.v;
            const float area = s1 * t2 - s2 * t1;
            if (std::fabs(area) < 1e-9f) {
                continue;
            }
            float dPdu[3], dPdv[3];
            for (int k = 0; k < 3; ++k) {
                dPdu[k] = (t2 * e1[k] - t1 * e2[k]) / area;
                dPdv[k] = (s1 * e2[k] - s2 * e1[k]) / area;
            }
            for (const uint32_t vi : idx) {
                const Vertex vx = vertexAt(g, vi);
                const float n[3] = {vx.nx, vx.ny, vx.nz};
                const float t[3] = {vx.tx, vx.ty, vx.tz};
                auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
                auto projectedUnit = [&](const float* d, float* out) {
                    const float nd = dot(n, d);
                    for (int k = 0; k < 3; ++k) out[k] = d[k] - n[k] * nd;
                    const float len = std::sqrt(dot(out, out));
                    if (len < 1e-4f * std::sqrt(dot(d, d)) || len == 0.0f) return false;
                    for (int k = 0; k < 3; ++k) out[k] /= len;
                    return true;
                };
                float pu[3], pv[3];
                if (!projectedUnit(dPdu, pu) || !projectedUnit(dPdv, pv)) {
                    continue;
                }
                ++tested;
                const float len = std::sqrt(dot(t, t));
                if (!near(len, 1.0f, 1e-3f)) ++badLength;
                const float ortho = std::fabs(dot(t, n));
                worstOrtho = std::max(worstOrtho, ortho);
                if (ortho > 1e-3f) ++badOrtho;
                // The per-vertex tangent averages the triangles around it, so it
                // need not match any one triangle exactly; 60 degrees is far inside
                // what a reversed or axis-swapped tangent produces (180 or 90).
                const float alongU = dot(t, pu) / (len > 0.0f ? len : 1.0f);
                worstU = std::min(worstU, alongU);
                if (!(alongU > 0.5f)) ++badU;
                const float bt[3] = {
                    (n[1] * t[2] - n[2] * t[1]) * vx.tw,
                    (n[2] * t[0] - n[0] * t[2]) * vx.tw,
                    (n[0] * t[1] - n[1] * t[0]) * vx.tw};
                const float btLen = std::sqrt(dot(bt, bt));
                // Toward v = 0, the image's TOP row, which is where a normal map's
                // green channel points (see the header comment above).
                const float alongV = btLen > 0.0f ? -dot(bt, pv) / btLen : -1.0f;
                worstV = std::min(worstV, alongV);
                if (!(alongV > 0.5f) || !near(std::fabs(vx.tw), 1.0f)) ++badV;
            }
        }
        std::cout << "  " << name << ": " << tested << " corners, worst dot(t, +u) " << worstU
                  << ", worst dot(cross(n, t) * w, +v) " << worstV << ", worst |dot(t, n)| " << worstOrtho << '\n';
        const std::string prefix = std::string(name) + ": ";
        check(tested > 0, (prefix + "has tangent-frame corners to test").c_str());
        check(badLength == 0, (prefix + "tangent is unit length (" + std::to_string(badLength) + " bad)").c_str());
        check(badOrtho == 0, (prefix + "tangent is orthogonal to the normal (" + std::to_string(badOrtho) + " bad)").c_str());
        check(badU == 0, (prefix + "tangent points along +u (" + std::to_string(badU) + " bad)").c_str());
        check(badV == 0, (prefix + "cross(n, t) * w points along +v (" + std::to_string(badV) + " bad)").c_str());
    }

    // UV1 is the LIGHTMAP unwrap. The box, cylinder, cone and capsule carry upstream's
    // (every face or part in its own padded cell); until 2026-09-23 every primitive
    // copied UV0 into UV1, so a baked box wrote all six faces into one square and each
    // face showed a blend of them. A part's cell must lie inside [0, 1] and overlap no
    // other part's cell, or two surfaces share lightmap texels.
    {
        struct Rect { float minU = 2.0f, minV = 2.0f, maxU = -1.0f, maxV = -1.0f; };
        const auto grow = [](Rect& r, const float u, const float v) {
            r.minU = std::min(r.minU, u); r.maxU = std::max(r.maxU, u);
            r.minV = std::min(r.minV, v); r.maxV = std::max(r.maxV, v);
        };
        const auto inside = [](const Rect& r) {
            return r.minU >= 0.0f && r.minV >= 0.0f && r.maxU <= 1.0f && r.maxV <= 1.0f;
        };
        // Strictly disjoint: the padding keeps a gap, so touching is a failure too.
        const auto disjoint = [](const Rect& a, const Rect& b) {
            return a.maxU < b.minU || b.maxU < a.minU || a.maxV < b.minV || b.maxV < a.minV;
        };
        const auto checkCells = [&](const char* name, const std::vector<Rect>& cells) {
            int outside = 0;
            int overlaps = 0;
            for (size_t i = 0; i < cells.size(); ++i) {
                outside += inside(cells[i]) ? 0 : 1;
                for (size_t j = i + 1; j < cells.size(); ++j) {
                    overlaps += disjoint(cells[i], cells[j]) ? 0 : 1;
                }
            }
            const std::string prefix = std::string(name) + ": ";
            check(outside == 0, (prefix + "every UV1 cell lies inside [0, 1] (" + std::to_string(outside) + " outside)").c_str());
            check(overlaps == 0, (prefix + "no two UV1 cells overlap (" + std::to_string(overlaps) + " overlapping pairs)").c_str());
        };

        // Box: six faces of four vertices each, in generation order.
        const PrimitiveGeometry box = createBoxGeometry();
        check(box.uvs1.size() == box.uvs.size(), "box: carries its own UV1");
        std::vector<Rect> faces(6);
        for (size_t vertex = 0; vertex * 2 < box.uvs1.size(); ++vertex) {
            grow(faces[std::min<size_t>(vertex / 4, 5)], box.uvs1[vertex * 2], box.uvs1[vertex * 2 + 1]);
        }
        checkCells("box faces", faces);

        // Cylinder: the body and the two flat caps, told apart by their normals.
        const PrimitiveGeometry cylinder = createCylinderGeometry();
        check(cylinder.uvs1.size() == cylinder.uvs.size(), "cylinder: carries its own UV1");
        std::vector<Rect> parts(3);
        for (size_t vertex = 0; vertex * 2 < cylinder.uvs1.size(); ++vertex) {
            const float ny = cylinder.normals[vertex * 3 + 1];
            const size_t part = ny > 0.5f ? 2 : (ny < -0.5f ? 1 : 0);
            grow(parts[part], cylinder.uvs1[vertex * 2], cylinder.uvs1[vertex * 2 + 1]);
        }
        checkCells("cylinder body and caps", parts);

        for (const auto& [name, geometry] : std::vector<std::pair<const char*, PrimitiveGeometry>>{
                 {"cone", createConeGeometry()}, {"capsule", createCapsuleGeometry()}}) {
            Rect all;
            for (size_t i = 0; i + 1 < geometry.uvs1.size(); i += 2) {
                grow(all, geometry.uvs1[i], geometry.uvs1[i + 1]);
            }
            check(geometry.uvs1.size() == geometry.uvs.size() && inside(all),
                (std::string(name) + ": carries its own UV1, inside [0, 1]").c_str());
        }

        // Upstream's plane and sphere use UV0 as UV1; an empty uvs1 says so.
        check(createPlaneGeometry().uvs1.empty(), "plane: UV1 is its UV0 (no unwrap of its own)");
        check(createSphereGeometry().uvs1.empty(), "sphere: UV1 is its UV0 (no unwrap of its own)");
    }

    std::cout << (failures == 0 ? "PASS\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
