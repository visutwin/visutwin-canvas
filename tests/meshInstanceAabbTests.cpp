// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.11.2026
//
// MeshInstance::aabb() keeps its world bounds until something they are built from
// changes: the node (or an ancestor) moving, the mesh bounds, a custom local box, the
// skin instance, and for a skinned instance without custom bounds any bone moving or
// the bones being replaced. Every camera, shadow camera and cascade reads the bounds,
// so a skinned instance no longer walks its bones per read; each case here is one the
// cache must NOT serve stale.

#include <iostream>
#include <memory>
#include <vector>

#include "core/shape/boundingBox.h"
#include "scene/graphNode.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/skin.h"
#include "scene/skinInstance.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr float kTolerance = 1e-5f;

    bool centredAt(const BoundingBox& box, const float x, const float y, const float z)
    {
        return nearStrict(box.center(), x, y, z, kTolerance);
    }

    void meshBoundsTests()
    {
        std::cout << "\nMesh bounds\n";
        auto mesh = std::make_shared<Mesh>();
        mesh->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f)));
        StandardMaterial material;
        GraphNode parent;
        auto* node = new GraphNode();
        parent.addChild(node);
        MeshInstance instance(mesh.get(), &material, node);

        check(centredAt(instance.aabb(), 0.0f, 0.0f, 0.0f), "the bounds start at the mesh's");
        node->setLocalPosition(2.0f, 0.0f, 0.0f);
        check(centredAt(instance.aabb(), 2.0f, 0.0f, 0.0f), "they follow the node");
        parent.setLocalPosition(0.0f, 3.0f, 0.0f);
        check(centredAt(instance.aabb(), 2.0f, 3.0f, 0.0f), "and a parent");
        parent.setLocalPosition(0.0f, 5.0f, 0.0f);
        check(centredAt(instance.aabb(), 2.0f, 5.0f, 0.0f), "and the parent again");
        mesh->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 1.0f), Vector3(1.0f, 1.0f, 1.0f)));
        check(centredAt(instance.aabb(), 2.0f, 5.0f, 1.0f), "and the mesh bounds");

        const BoundingBox custom(Vector3(0.0f, 0.0f, -4.0f), Vector3(0.5f, 0.5f, 0.5f));
        instance.setLocalCustomAabb(&custom);
        check(centredAt(instance.aabb(), 2.0f, 5.0f, -4.0f), "a custom local box replaces the mesh's");
        node->setLocalPosition(0.0f, 0.0f, 0.0f);
        check(centredAt(instance.aabb(), 0.0f, 5.0f, -4.0f), "and follows the node");
        instance.setLocalCustomAabb(nullptr);
        check(centredAt(instance.aabb(), 0.0f, 5.0f, 1.0f), "clearing it gives the mesh's back");
    }

    void skinBoundsTests()
    {
        std::cout << "\nSkinned bounds\n";
        auto mesh = std::make_shared<Mesh>();
        mesh->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f)));
        StandardMaterial material;
        GraphNode root;
        auto* node = new GraphNode();
        auto* boneParent = new GraphNode();
        auto* bone = new GraphNode();
        auto* otherBone = new GraphNode();
        root.addChild(node);
        root.addChild(boneParent);
        boneParent->addChild(bone);
        root.addChild(otherBone);
        otherBone->setLocalPosition(0.0f, 0.0f, 7.0f);

        auto skin = std::make_shared<Skin>(std::vector<Matrix4>{Matrix4::identity()}, std::vector<std::string>{"bone"});
        skin->setBoneAabbs({BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f))}, {1});
        auto skinInstance = std::make_shared<SkinInstance>(skin);
        skinInstance->setBones({bone});

        MeshInstance instance(mesh.get(), &material, node);
        node->setLocalPosition(9.0f, 0.0f, 0.0f);
        check(centredAt(instance.aabb(), 9.0f, 0.0f, 0.0f), "unskinned, the bounds are the node's");
        instance.setSkinInstance(skinInstance);
        check(centredAt(instance.aabb(), 0.0f, 0.0f, 0.0f), "a skin instance moves them to the bones at once");

        bone->setLocalPosition(1.0f, 0.0f, 0.0f);
        check(centredAt(instance.aabb(), 1.0f, 0.0f, 0.0f), "a bone moving moves them");
        boneParent->setLocalPosition(0.0f, 2.0f, 0.0f);
        check(centredAt(instance.aabb(), 1.0f, 2.0f, 0.0f), "and a bone's parent");
        boneParent->setLocalPosition(0.0f, 4.0f, 0.0f);
        check(centredAt(instance.aabb(), 1.0f, 4.0f, 0.0f), "and the bone's parent again");
        node->setLocalPosition(0.0f, 0.0f, 0.0f);
        check(centredAt(instance.aabb(), 1.0f, 4.0f, 0.0f), "the node moving does not move bone bounds");

        skinInstance->setBones({otherBone});
        check(centredAt(instance.aabb(), 0.0f, 0.0f, 7.0f), "replacing the bones moves them to the new bone");

        const BoundingBox custom(Vector3(0.0f, -3.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
        instance.setLocalCustomAabb(&custom);
        check(centredAt(instance.aabb(), 0.0f, -3.0f, 0.0f), "a custom local box wins over the bones");
        otherBone->setLocalPosition(0.0f, 0.0f, 8.0f);
        check(centredAt(instance.aabb(), 0.0f, -3.0f, 0.0f), "and the bones no longer move it");
        instance.setLocalCustomAabb(nullptr);
        check(centredAt(instance.aabb(), 0.0f, 0.0f, 8.0f), "clearing it gives the bones' back");

        instance.setSkinInstance(nullptr);
        check(centredAt(instance.aabb(), 0.0f, 0.0f, 0.0f), "removing the skin gives the node's back");
    }
}

int main()
{
    meshBoundsTests();
    skinBoundsTests();
    return finish("MeshInstance world bounds");
}
