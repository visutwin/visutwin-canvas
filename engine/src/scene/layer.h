// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <algorithm>
#include <functional>
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
        Layer(const std::string& name, int id): _id(id), _name(name) {}

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

        /**
         * How this layer orders its opaque and its transparent draws. The defaults
         * are upstream's and are what the renderer always did: group the opaque pass
         * by material to keep state changes down, and draw the transparent pass
         * farthest-first so it composites.
         */
        SortMode opaqueSortMode() const { return _opaqueSortMode; }
        void setOpaqueSortMode(const SortMode value) { _opaqueSortMode = value; }

        SortMode transparentSortMode() const { return _transparentSortMode; }
        void setTransparentSortMode(const SortMode value) { _transparentSortMode = value; }

        /**
         * Comparator used when the mode is SORTMODE_CUSTOM — a strict weak ordering
         * over mesh instances, as std::sort requires. Ignored in every other mode,
         * and a null callback in SORTMODE_CUSTOM leaves the order alone rather than
         * falling back to something the caller did not ask for.
         *
         * The sort distance is computed before it runs, so a callback may read
         * MeshInstance::sortDistance.
         */
        using SortCallback = std::function<bool(const MeshInstance*, const MeshInstance*)>;
        const SortCallback& customSortCallback() const { return _customSortCallback; }
        void setCustomSortCallback(SortCallback callback) { _customSortCallback = std::move(callback); }

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
        SortMode _opaqueSortMode = SortMode::SORTMODE_MATERIALMESH;
        SortMode _transparentSortMode = SortMode::SORTMODE_BACK2FRONT;
        SortCallback _customSortCallback;

        bool _enabled = true;
        bool _clearColorBuffer = false;
        bool _clearDepthBuffer = false;
        bool _clearStencilBuffer = false;
        bool _dirtyComposition = false;
    };
}
