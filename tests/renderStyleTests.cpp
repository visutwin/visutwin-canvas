// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.10.2026
//
// A mesh drawn in the wireframe style draws a line list holding every edge of its triangles
// once. Nothing on screen tells a missing edge from a doubled one, so the line list is held
// here against a brute-force edge set of the solid triangles:
//
//  - Mesh::generateWireframe writes render style 1: lines, indexed, each edge once, every
//    edge of every triangle present, in the source's index format;
//  - a mesh without an index buffer gets each triangle's three edges;
//  - a style the mesh has no geometry for draws solid (Mesh::drawStyle);
//  - MeshInstance::setRenderStyle builds the line list, and a render component hands its
//    style to the instances it has and the ones it gets later.

#include <algorithm>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/constants.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    std::vector<uint32_t> indicesOf(const IndexBuffer& buffer)
    {
        std::vector<uint32_t> out(static_cast<size_t>(buffer.numIndices()));
        const auto& bytes = buffer.storage();
        for (size_t i = 0; i < out.size(); ++i) {
            if (buffer.format() == INDEXFORMAT_UINT16) {
                uint16_t v;
                std::memcpy(&v, bytes.data() + i * 2, 2);
                out[i] = v;
            } else if (buffer.format() == INDEXFORMAT_UINT32) {
                std::memcpy(&out[i], bytes.data() + i * 4, 4);
            } else {
                out[i] = bytes[i];
            }
        }
        return out;
    }

    using Edge = std::pair<uint32_t, uint32_t>;

    Edge edge(const uint32_t a, const uint32_t b) { return a < b ? Edge{a, b} : Edge{b, a}; }

    // Holds the wireframe of `mesh` against the edges of its solid triangles.
    void checkWireframe(const char* name, Mesh& mesh)
    {
        mesh.generateWireframe();
        const Primitive lines = mesh.getPrimitive(RENDERSTYLE_WIREFRAME);
        const auto wireIb = mesh.getIndexBuffer(RENDERSTYLE_WIREFRAME);
        check(lines.type == PRIMITIVE_LINES && lines.indexed, std::string(name) + ": an indexed line list");
        check(wireIb != nullptr && lines.count == wireIb->numIndices() && lines.count % 2 == 0,
            std::string(name) + ": counting its own index buffer, two indices a line");
        if (!wireIb) {
            return;
        }

        std::set<Edge> expected;
        if (const auto solidIb = mesh.getIndexBuffer(RENDERSTYLE_SOLID)) {
            check(wireIb->format() == solidIb->format(), std::string(name) + ": in the source's index format");
            const auto tri = indicesOf(*solidIb);
            for (size_t i = 0; i + 2 < tri.size(); i += 3) {
                expected.insert(edge(tri[i], tri[i + 1]));
                expected.insert(edge(tri[i + 1], tri[i + 2]));
                expected.insert(edge(tri[i + 2], tri[i]));
            }
        }

        const auto wire = indicesOf(*wireIb);
        std::set<Edge> got;
        bool unique = true;
        for (size_t i = 0; i + 1 < wire.size(); i += 2) {
            unique = got.insert(edge(wire[i], wire[i + 1])).second && unique;
        }
        check(unique, std::string(name) + ": no edge twice");
        check(got == expected, std::string(name) + ": every edge of every triangle, and nothing else");
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>(StubGraphicsDevice::Options{
        .cpuBuffers = true, .keepIndexData = true});

    std::cout << "indexed meshes\n";
    auto box = createMeshFromGeometry(device, createBoxGeometry());
    check(box != nullptr, "a box mesh");
    checkWireframe("box", *box);
    // Six faces of two triangles: four border edges and a diagonal each.
    check(box->getPrimitive(RENDERSTYLE_WIREFRAME).count == 6 * 5 * 2, "box: 30 edges, 60 indices");
    auto sphere = createMeshFromGeometry(device, createSphereGeometry());
    checkWireframe("sphere", *sphere);
    auto capsule = createMeshFromGeometry(device, createCapsuleGeometry());
    checkWireframe("capsule", *capsule);

    std::cout << "\nunindexed mesh\n";
    {
        auto mesh = std::make_shared<Mesh>();
        mesh->setVertexBuffer(box->getVertexBuffer());
        Primitive solid;
        solid.count = 6;   // two triangles of the box's vertices, unindexed
        mesh->setPrimitive(solid);
        mesh->generateWireframe();
        const auto ib = mesh->getIndexBuffer(RENDERSTYLE_WIREFRAME);
        check(ib != nullptr, "gets an index buffer");
        const int triangles = box->getVertexBuffer()->numVertices() / 3;
        check(ib && ib->numIndices() == triangles * 6, "three edges for each whole triangle of its vertices");
        const auto wire = ib ? indicesOf(*ib) : std::vector<uint32_t>{};
        check(wire.size() >= 6 && wire[0] == 0 && wire[1] == 1 && wire[2] == 1 && wire[3] == 2
            && wire[4] == 2 && wire[5] == 0, "triangle 0 as 0-1, 1-2, 2-0");
    }

    std::cout << "\ndraw style\n";
    {
        auto plain = createMeshFromGeometry(device, createBoxGeometry());
        check(plain->drawStyle(RENDERSTYLE_WIREFRAME) == RENDERSTYLE_SOLID,
            "a mesh with no line list draws solid in the wireframe style");
        check(box->drawStyle(RENDERSTYLE_WIREFRAME) == RENDERSTYLE_WIREFRAME, "one with a line list draws it");
        MeshInstance instance(plain.get(), nullptr);
        instance.setRenderStyle(RENDERSTYLE_WIREFRAME);
        check(plain->drawStyle(RENDERSTYLE_WIREFRAME) == RENDERSTYLE_WIREFRAME,
            "setting the style on an instance builds its mesh's line list");
    }

    std::cout << "\nrender component\n";
    {
        auto engine = makeTestEngine<RenderComponentSystem>(device);
        auto owned = std::make_unique<Entity>();
        Entity* entity = owned.get();
        entity->setEngine(engine.get());
        engine->root()->addChild(std::move(owned));
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        render->setType("cone");
        render->setRenderStyle(RENDERSTYLE_WIREFRAME);
        check(!render->meshInstances().empty() && render->meshInstances()[0]->renderStyle() == RENDERSTYLE_WIREFRAME,
            "the component's instances take its style");
        auto cylinder = createMeshFromGeometry(device, createCylinderGeometry());
        MeshInstance* added = render->addMeshInstance(std::make_unique<MeshInstance>(cylinder.get(), nullptr, entity));
        check(added->renderStyle() == RENDERSTYLE_WIREFRAME && cylinder->drawStyle(RENDERSTYLE_WIREFRAME)
            == RENDERSTYLE_WIREFRAME, "an instance added later takes it, its mesh gets a line list");
        render->clearMeshInstances();
    }

    return finish("render style");
}
