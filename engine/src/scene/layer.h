// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "meshInstance.h"

namespace visutwin::canvas
{
    /**
     * A Layer represents a renderable subset of the scene. It can contain a list of mesh instances,
     * lights and cameras, their render settings and also defines custom callbacks before, after or
     * during rendering. Layers are organized inside {@link LayerComposition} in a desired order.
     */
    class Layer
    {
    public:
        Layer(const std::string& name, int id): _name(name), _id(id) {}

        int id() const { return _id; }

        const std::string& name() const { return _name; }

        void addMeshInstances(const std::vector<MeshInstance*>& meshInstances)
        {
            for (auto* meshInstance : meshInstances) {
                if (!meshInstance) {
                    continue;
                }
                if (std::find(_meshInstances.begin(), _meshInstances.end(), meshInstance) == _meshInstances.end()) {
                    _meshInstances.push_back(meshInstance);
                }
            }
        }

        void removeMeshInstances(const std::vector<MeshInstance*>& meshInstances)
        {
            for (auto* meshInstance : meshInstances) {
                if (!meshInstance) {
                    continue;
                }
                _meshInstances.erase(std::remove(_meshInstances.begin(), _meshInstances.end(), meshInstance), _meshInstances.end());
            }
        }

        const std::vector<MeshInstance*>& meshInstances() const { return _meshInstances; }

        bool enabled() const { return _enabled; }
        void setEnabled(const bool value) { _enabled = value; _dirtyComposition = true; }

        bool clearColorBuffer() const { return _clearColorBuffer; }
        void setClearColorBuffer(const bool value) { _clearColorBuffer = value; _dirtyComposition = true; }

        bool clearDepthBuffer() const { return _clearDepthBuffer; }
        void setClearDepthBuffer(const bool value) { _clearDepthBuffer = value; _dirtyComposition = true; }

        bool clearStencilBuffer() const { return _clearStencilBuffer; }
        void setClearStencilBuffer(const bool value) { _clearStencilBuffer = value; _dirtyComposition = true; }

        bool dirtyComposition() const { return _dirtyComposition; }
        void setDirtyComposition(const bool value) { _dirtyComposition = value; }

    private:
        int _id;

        std::string _name;
        std::vector<MeshInstance*> _meshInstances;
        bool _enabled = true;
        bool _clearColorBuffer = false;
        bool _clearDepthBuffer = false;
        bool _clearStencilBuffer = false;
        bool _dirtyComposition = false;
    };
}
