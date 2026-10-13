// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Immediate lines and the WireRenderer, on a stub device that keeps its vertex buffers:
//
//  - A line goes into the batch for its (layer, depth test), as position and colour per vertex
//    in the 28-byte layout; drawLine, drawLines and drawLineArrays write exactly what they were
//    given.
//  - prepareFrame registers the batch's mesh instance with its layer, uploads the frame's
//    vertices into a ring slot per frame in flight (consecutive frames into different buffers)
//    and draws exactly the vertices written; clear hides it and empties the batch, and a batch
//    that stays nearly empty for 100 frames shrinks back.
//  - Every WireRenderer shape allocates exactly the vertices it writes (upstream's counts), in
//    the renderer's colour, through its transform; axes are red, green and blue whatever the
//    colour; a light is drawn in its own colour; a frustum's corners are the projection's.
//  - The line material says per draw whether the target takes linear HDR.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/math/matrix4.h"
#include "core/shape/boundingBox.h"
#include "framework/extras/wireRenderer.h"
#include "platform/graphics/vertexBuffer.h"
#include "scene/composition/layerComposition.h"
#include "scene/immediate/immediate.h"
#include "scene/immediate/immediateBatch.h"
#include "scene/layer.h"
#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr size_t kFloats = LineWriter::kFloatsPerVertex;

    bool sameColor(const float* v, const Color& c)
    {
        return v[3] == c.r && v[4] == c.g && v[5] == c.b && v[6] == c.a;
    }

    bool samePoint(const float* v, const Vector3& p, const float epsilon = 1e-5f)
    {
        return std::abs(v[0] - p.getX()) < epsilon && std::abs(v[1] - p.getY()) < epsilon &&
            std::abs(v[2] - p.getZ()) < epsilon;
    }

    // The vertices a shape writes into the default batch.
    size_t written(Immediate& immediate, const size_t before)
    {
        return immediate.batch(LAYERID_IMMEDIATE, true).vertexCount() - before;
    }
}

int main()
{
    std::cout << std::unitbuf;
    const auto device = std::make_shared<StubGraphicsDevice>(
        StubGraphicsDevice::Options{.cpuBuffers = true, .framesInFlight = 3});

    std::cout << "lines into batches\n";
    {
        Immediate immediate(device);
        const Color red(1.0f, 0.0f, 0.0f, 1.0f);
        immediate.drawLine(Vector3(1.0f, 2.0f, 3.0f), Vector3(4.0f, 5.0f, 6.0f), red);
        ImmediateBatch& batch = immediate.batch(LAYERID_IMMEDIATE, true);
        check(batch.vertexCount() == 2 && samePoint(batch.vertices(), Vector3(1.0f, 2.0f, 3.0f)) &&
                  samePoint(batch.vertices() + kFloats, Vector3(4.0f, 5.0f, 6.0f)) &&
                  sameColor(batch.vertices(), red) && sameColor(batch.vertices() + kFloats, red),
            "drawLine writes both ends with the colour, position then colour per vertex");

        const Color a(0.1f, 0.2f, 0.3f, 0.4f);
        const Color b(0.5f, 0.6f, 0.7f, 0.8f);
        immediate.drawLines({Vector3(0.0f), Vector3(1.0f, 0.0f, 0.0f)}, {a, b}, false);
        ImmediateBatch& noDepth = immediate.batch(LAYERID_IMMEDIATE, false);
        check(immediate.batchCount() == 2 && noDepth.vertexCount() == 2 && batch.vertexCount() == 2,
            "a line without the depth test goes into a batch of its own");
        check(sameColor(noDepth.vertices(), a) && sameColor(noDepth.vertices() + kFloats, b),
            "drawLines writes a colour per point");

        const float packed[6] = {0, 0, 0, 0, 1, 0};
        const float colors[8] = {1, 1, 1, 1, 0, 0, 0, 0.5f};
        immediate.drawLineArrays(packed, 6, colors, red, true, LAYERID_WORLD);
        ImmediateBatch& world = immediate.batch(LAYERID_WORLD, true);
        check(immediate.batchCount() == 3 && world.vertexCount() == 2 &&
                  samePoint(world.vertices() + kFloats, Vector3(0.0f, 1.0f, 0.0f)) &&
                  sameColor(world.vertices() + kFloats, Color(0.0f, 0.0f, 0.0f, 0.5f)),
            "drawLineArrays reads packed positions and rgba, into the layer's batch");
    }

    std::cout << "\nthe frame: upload, draw, clear\n";
    {
        Immediate immediate(device);
        LayerComposition composition;
        auto immediateLayer = std::make_shared<Layer>("Immediate", LAYERID_IMMEDIATE);
        composition.pushOpaque(immediateLayer);
        composition.pushTransparent(immediateLayer);

        immediate.drawLine(Vector3(0.0f), Vector3(1.0f), Color(1.0f, 1.0f, 1.0f, 1.0f));
        immediate.prepareFrame(&composition);
        ImmediateBatch& batch = immediate.batch(LAYERID_IMMEDIATE, true);
        MeshInstance* instance = batch.meshInstance();
        const auto& layerInstances = immediateLayer->meshInstances();
        check(std::find(layerInstances.begin(), layerInstances.end(), instance) != layerInstances.end(),
            "the batch's mesh instance joins its layer");
        check(instance->visible() && !instance->cull() && !instance->castShadow(),
            "and is visible, never culled and casts no shadow");
        const Primitive& primitive = batch.mesh()->getPrimitive(0);
        auto first = batch.mesh()->getVertexBuffer();
        check(primitive.type == PRIMITIVE_LINES && primitive.count == 2 && !primitive.indexed && first &&
                  first->format()->size() == 28,
            "it draws the frame's two vertices as lines, 28 bytes each");
        float uploaded[kFloats * 2];
        std::memcpy(uploaded, first->storage().data(), sizeof(uploaded));
        check(samePoint(uploaded + kFloats, Vector3(1.0f)), "the buffer holds the vertices written");
        check(instance->material() && instance->material()->transparent(), "with the alpha-blended line material");

        immediate.clear();
        check(batch.vertexCount() == 0 && !instance->visible(), "clear empties the batch and hides it");

        // Three frames in flight: three different buffers, then the first again.
        std::vector<VertexBuffer*> buffers;
        for (int frame = 0; frame < 4; ++frame) {
            immediate.drawLine(Vector3(static_cast<float>(frame)), Vector3(1.0f));
            immediate.prepareFrame(&composition);
            buffers.push_back(batch.mesh()->getVertexBuffer().get());
            immediate.clear();
        }
        check(buffers[0] != buffers[1] && buffers[1] != buffers[2] && buffers[0] != buffers[2],
            "consecutive frames upload into different buffers of the ring");
        check(buffers[3] == buffers[0], "and the ring comes back round after one slot per frame in flight");

        immediate.prepareFrame(&composition);
        check(!instance->visible(), "a frame with no lines draws nothing");

        // Grow, then go quiet: the storage shrinks back after 100 frames.
        for (int i = 0; i < 1000; ++i) {
            immediate.drawLine(Vector3(0.0f), Vector3(1.0f));
        }
        immediate.prepareFrame(&composition);
        immediate.clear();
        const size_t grown = batch.capacity();
        for (int frame = 0; frame < ImmediateBatch::kShrinkFrameDelay; ++frame) {
            immediate.drawLine(Vector3(0.0f), Vector3(1.0f));
            immediate.prepareFrame(&composition);
            immediate.clear();
        }
        check(grown >= 2000 && batch.capacity() == ImmediateBatch::kMinVertexCapacity,
            "a batch that stays nearly empty for 100 frames gives its storage back");
    }

    std::cout << "\nthe line material and the target\n";
    {
        Immediate immediate(device);
        immediate.drawLine(Vector3(0.0f), Vector3(1.0f));
        const Material* material = immediate.batch(LAYERID_IMMEDIATE, true).meshInstance()->material();
        size_t size = 0;
        device->setHdrPass(false);
        const auto* gamma = static_cast<const float*>(material->customUniformData(size));
        const bool gammaTarget = size == 16 && gamma[0] == 0.0f;
        device->setHdrPass(true);
        const auto* hdr = static_cast<const float*>(material->customUniformData(size));
        check(gammaTarget && hdr[0] == 1.0f, "the block says, per draw, whether the target takes linear HDR");
        device->setHdrPass(false);
        immediate.clear();
    }

    std::cout << "\nWireRenderer shapes\n";
    {
        Scene scene(device);
        Immediate& immediate = *scene.immediate();
        WireRenderer wire(&scene);
        wire.setSegments(16);
        const Color color(0.2f, 0.4f, 0.6f, 1.0f);
        wire.setColor(color);

        auto run = [&](const char* name, const size_t vertices, auto&& draw) {
            const size_t before = immediate.batch(LAYERID_IMMEDIATE, true).vertexCount();
            draw();
            check(written(immediate, before) == vertices, std::string(name) + " writes upstream's vertex count");
        };
        const Vector3 o(0.0f), x(1.0f, 0.0f, 0.0f), y(0.0f, 1.0f, 0.0f);
        run("line", 2, [&] { wire.line(o, x); });
        run("sphere", 16 * 3 * 2, [&] { wire.sphere(o, 1.0f); });
        run("circle", 16 * 2, [&] { wire.circle(o, y, 1.0f); });
        run("cylinder", (16 * 2 + 4) * 2, [&] { wire.cylinder(o, y, 1.0f); });
        run("capsule", (16 * 2 + 4 + 8 * 4) * 2, [&] { wire.capsule(o, y, 1.0f); });
        run("cone", (16 + 4) * 2, [&] { wire.cone(o, y, 30.0f, 2.0f); });
        run("plane", 5 * 2, [&] { wire.plane(o, y, 2.0f); });
        run("point", 3 * 2, [&] { wire.point(o, 1.0f); });
        run("arrow", (16 + 5) * 2, [&] { wire.arrow(o, x); });
        run("boxMinMax", 12 * 2, [&] { wire.boxMinMax(o, Vector3(1.0f)); });
        run("box", 12 * 2, [&] { wire.box(BoundingBox(o, Vector3(1.0f))); });
        const std::vector<Vector3> four = {o, x, y, Vector3(1.0f, 1.0f, 0.0f)};
        run("lines", 4, [&] { wire.lines(four); });
        run("polyline", 3 * 2, [&] { wire.polyline(four); });
        run("loop", 4 * 2, [&] { wire.loop(four); });
        run("axes", 3 * 2, [&] { wire.axes(Matrix4::identity(), 1.0f); });
        run("frustum", 12 * 2, [&] { wire.frustum(Matrix4::perspective(60.0f, 1.0f, 1.0f, 10.0f)); });
        run("degenerate cylinder", 0, [&] { wire.cylinder(o, o, 1.0f); });

        ImmediateBatch& batch = immediate.batch(LAYERID_IMMEDIATE, true);
        immediate.clear();

        wire.line(o, x);
        check(sameColor(batch.vertices(), color) && sameColor(batch.vertices() + kFloats, color),
            "shapes are drawn in the renderer's colour");
        immediate.clear();

        wire.setTransform(Matrix4::translation(Vector3(10.0f, 0.0f, 0.0f)));
        wire.line(o, x);
        check(samePoint(batch.vertices(), Vector3(10.0f, 0.0f, 0.0f)) &&
                  samePoint(batch.vertices() + kFloats, Vector3(11.0f, 0.0f, 0.0f)),
            "every point goes through the transform");
        wire.clearTransform();
        immediate.clear();

        wire.axes(Matrix4::translation(Vector3(0.0f, 0.0f, 5.0f)), 2.0f);
        check(sameColor(batch.vertices(), Color(1.0f, 0.0f, 0.0f, 1.0f)) &&
                  sameColor(batch.vertices() + kFloats * 2, Color(0.0f, 1.0f, 0.0f, 1.0f)) &&
                  sameColor(batch.vertices() + kFloats * 4, Color(0.0f, 0.0f, 1.0f, 1.0f)) &&
                  samePoint(batch.vertices(), Vector3(0.0f, 0.0f, 5.0f)) &&
                  samePoint(batch.vertices() + kFloats * 3, Vector3(0.0f, 2.0f, 5.0f)),
            "axes are red, green and blue from the matrix's translation, whatever the colour");
        immediate.clear();

        // An orthographic box: its corners are the box the projection maps to clip space.
        wire.frustum(Matrix4::ortho(-2.0f, 2.0f, -1.0f, 1.0f, 1.0f, 5.0f));
        bool corners = true;
        for (size_t i = 0; i < 24; ++i) {
            const float* v = batch.vertices() + i * kFloats;
            corners = corners && std::abs(std::abs(v[0]) - 2.0f) < 1e-4f && std::abs(std::abs(v[1]) - 1.0f) < 1e-4f &&
                (std::abs(v[2] + 1.0f) < 1e-4f || std::abs(v[2] + 5.0f) < 1e-4f);
        }
        check(corners, "a frustum's corners are where the projection maps the clip-space cube");
        immediate.clear();

        WireRenderer xray(&scene);
        xray.setDepthTest(false);
        xray.line(o, x);
        check(immediate.batch(LAYERID_IMMEDIATE, false).vertexCount() == 2 && batch.vertexCount() == 0,
            "a second renderer without the depth test draws into the other batch");
        immediate.clear();
    }

    return finish("immediate lines");
}
