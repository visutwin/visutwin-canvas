// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.10.2025.
//

#pragma once

#include <memory>
#include <platform/graphics/indexBuffer.h>
#include <platform/graphics/vertexBuffer.h>
#include <scene/meshInstance.h>
#include <scene/materials/material.h>
#include <scene/mesh.h>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Scene;
    class Texture;
    class GraphNode;

    /**
     * A visual representation of the sky
     */
    class SkyMesh
    {
    public:
        SkyMesh(const std::shared_ptr<GraphicsDevice>& device, Scene* scene, GraphNode* node, Texture* texture, int type);
        ~SkyMesh();

        MeshInstance* meshInstance() const { return _meshInstance.get(); }

    private:
        std::shared_ptr<Mesh> createInfiniteMesh(const std::shared_ptr<GraphicsDevice>& device) const;
        std::shared_ptr<Mesh> createBoxMesh(const std::shared_ptr<GraphicsDevice>& device) const;
        std::shared_ptr<Mesh> createDomeMesh(const std::shared_ptr<GraphicsDevice>& device) const;
        std::shared_ptr<Mesh> createMeshByType(const std::shared_ptr<GraphicsDevice>& device, int type) const;

        Scene* _scene = nullptr;
        std::shared_ptr<Material> _material;
        std::shared_ptr<Mesh> _mesh;
        std::unique_ptr<MeshInstance> _meshInstance;
    };
}
