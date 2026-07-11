// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#pragma once

#include <memory>
#include <string>

#include "gsplatData.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class GraphNode;
    class Material;
    class Mesh;
    class MeshInstance;
    class Shader;
    class VertexBuffer;

    /**
     * GPU resources for a Gaussian splat set: the packed splat storage buffer, a
     * 4-vertex quad mesh (instanced once per splat), the premultiplied-alpha splat
     * material with its custom Metal shader, and the model-space bounds.
     *
     * Shared between all GSplatInstances created from it.
     */
    class GSplatResource : public std::enable_shared_from_this<GSplatResource>
    {
    public:
        GSplatResource(std::unique_ptr<GSplatData> data,
                       const std::shared_ptr<GraphicsDevice>& device);

        /** Load a 3DGS PLY and create the GPU resources (nullptr on failure). */
        static std::shared_ptr<GSplatResource> loadPly(const std::string& path,
            const std::shared_ptr<GraphicsDevice>& device);

        /**
         * Create a renderable MeshInstance (with an attached GSplatInstance and
         * per-instance depth sorter) parented to `node`.
         */
        std::unique_ptr<MeshInstance> createMeshInstance(GraphNode* node);

        const GSplatData& data() const { return *_data; }
        int numSplats() const { return _data->numSplats(); }
        const std::shared_ptr<VertexBuffer>& splatBuffer() const { return _splatBuffer; }
        const std::shared_ptr<GraphicsDevice>& device() const { return _device; }

    private:
        std::shared_ptr<GraphicsDevice> _device;
        std::unique_ptr<GSplatData> _data;
        std::shared_ptr<VertexBuffer> _splatBuffer;
        std::shared_ptr<Mesh> _quadMesh;
        std::shared_ptr<Material> _material;
        std::shared_ptr<Shader> _shader;
    };
}
