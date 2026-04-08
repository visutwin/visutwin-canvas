// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 21.12.2025.
//
#include "meshInstance.h"

namespace visutwin::canvas
{
    MeshInstance::MeshInstance(Mesh* mesh, Material* material, GraphNode* node)
        : _material(material), _mesh(mesh), _node(node)
    {
    }

    BoundingBox MeshInstance::aabb()
    {
        // Use specified world space aabb
        if (!_updateAabb) {
            return _aabb;
        }

        // Callback function returning world space aabb
        if (_updateAabbFunc) {
            return _updateAabbFunc(_aabb);
        }

        // Use local space override aabb if specified
        BoundingBox localAabbStorage;
        BoundingBox* localAabb = _customAabb;
        bool toWorldSpace = (localAabb != nullptr);

        // Otherwise evaluate local aabb
        if (!localAabb) {
            localAabb = &localAabbStorage;

            if (_skinInstance) {
                if (_mesh) {
                    localAabb->setCenter(_mesh->aabb().center());
                    localAabb->setHalfExtents(_mesh->aabb().halfExtents());
                } else {
                    localAabb->setCenter(0, 0, 0);
                    localAabb->setHalfExtents(0, 0, 0);
                }
                // Note: skinned AABB calculation not yet implemented.
                // Requires accessing bone AABBs and transforming them.
                toWorldSpace = true;
            } else if (_node && (_aabbVer != _node->aabbVer() || (_mesh && _aabbMeshVer != _mesh->aabbVer()))) {
                // Local space bounding box from mesh
                if (_mesh) {
                    localAabb->setCenter(_mesh->aabb().center());
                    localAabb->setHalfExtents(_mesh->aabb().halfExtents());
                } else {
                    localAabb->setCenter(0, 0, 0);
                    localAabb->setHalfExtents(0, 0, 0);
                }

                // Note: morph target AABB expansion not yet implemented.
                // if (_mesh && _mesh->morph) {
                //     const BoundingBox& morphAabb = _mesh->morph->aabb;
                //     localAabb->_expand(morphAabb.getMin(), morphAabb.getMax());
                // }

                toWorldSpace = true;
                if (_node) {
                    _aabbVer = _node->aabbVer();
                }
                if (_mesh) {
                    _aabbMeshVer = _mesh->aabbVer();
                }
            }
        }

        // Store world space bounding box
        if (toWorldSpace && _node) {
            _aabb.setFromTransformedAabb(*localAabb, _node->worldTransform());
        }

        return _aabb;
    }
}
