// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.10.2025.
//
#pragma once

#include <memory>

#include "core/math/vector3.h"
#include "scene/constants.h"
#include "scene/graphNode.h"
#include "skyMesh.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Scene;
    class Texture;

    /**
     * Implementation of the sky.
     * Sky manages the sky mesh, type, and dome center.
     */
    class Sky
    {
    public:
        Sky(const std::shared_ptr<GraphicsDevice>& device, Scene* scene);
        ~Sky();

        void updateSkyMesh();
        void resetSkyMesh();
        SkyMesh* skyMesh();

        void setType(int value);
        int type() const { return _type; }

        void setDepthWrite(bool value);
        bool depthWrite() const { return _depthWrite; }

        /**
         * the tripod center for dome/box sky types.
         * The environment direction is computed relative to this point.
         * Ignored for SKYTYPE_INFINITE.
         */
        void setCenter(const Vector3& value) { _center = value; }
        const Vector3& center() const { return _center; }

        /**
         * Compute the world-space position of the sky dome center.
         * Transforms _center by the sky node's world transform.
         */
        Vector3 centerWorldPos() const;

        GraphNode* node() { return &_node; }

        /// Returns the 1×1 dummy texture used when atmosphere is enabled without envAtlas.
        /// Used by the renderer to bind a valid texture at fragment slot 2.
        Texture* atmosphereDummyTexture() const { return _atmosphereDummyTexture.get(); }

    private:
        std::shared_ptr<GraphicsDevice> _device;
        Scene* _scene = nullptr;
        int _type = SKYTYPE_INFINITE;
        bool _depthWrite = false;
        Vector3 _center = Vector3(0.0f, 1.0f, 0.0f);
        GraphNode _node = GraphNode("SkyMeshNode");
        std::unique_ptr<SkyMesh> _skyMesh;
        std::shared_ptr<Texture> _atmosphereDummyTexture; // 1×1 placeholder for atmosphere-only sky
    };
}
