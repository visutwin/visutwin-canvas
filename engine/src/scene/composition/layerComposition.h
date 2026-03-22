// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#pragma once

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "renderAction.h"
#include "core/eventHandler.h"

namespace visutwin::canvas
{
    /*
    * Layer Composition is a collection of Layer that is fed to Scene#layers to define rendering.
    */
    class LayerComposition : public EventHandler
    {
    public:
        LayerComposition(const std::string& name = "Untitled"): _name(name) {}
        ~LayerComposition();

        // Adds part of the layer with opaque (non semi-transparent) objects to the end of the layerList
        void pushOpaque(const std::shared_ptr<Layer>& layer);

        // Adds part of the layer with semi-transparent objects to the end of the layerList
        void pushTransparent(const std::shared_ptr<Layer>& layer);

        const std::vector<RenderAction*>& renderActions();
        std::shared_ptr<Layer> getLayerById(int layerId) const;
        std::shared_ptr<Layer> getLayerByName(const std::string& name) const;
        bool isEnabled(const Layer* layer, bool transparent) const;
        void markDirty() { _dirty = true; }

    private:
        bool isSublayerAdded(const std::shared_ptr<Layer>& layer, bool transparent) const;
        int getOpaqueIndex(const std::shared_ptr<Layer>& layer) const;
        int getTransparentIndex(const std::shared_ptr<Layer>& layer) const;

        void updateLayerMaps();
        void rebuildRenderActions();
        void clearRenderActions();

        std::string _name;

        // True if the composition needs to be updated before rendering.
        bool _dirty = true;

        std::vector<std::shared_ptr<Layer>> _layerList;

        // A read-only array of boolean values, matching layerList.
        // True means only semi-transparent objects are rendered, and false means opaque.
        std::vector<bool> _subLayerList;

        // A read-only array of boolean values, matching layerList.
        // True means the layer is rendered; false means it's skipped.
        std::vector<bool> _subLayerEnabled;

        std::vector<RenderAction*> _renderActions;
        size_t _lastCameraCount = 0;

        // Opaque sublayer order mapping (layer id -> index)
        std::unordered_map<int, int> _opaqueOrder;

        // Transparent sublayer order mapping (layer id -> index)
        std::unordered_map<int, int> _transparentOrder;

        // A mapping of Layer to its transparent index in layerList.
        std::map<std::shared_ptr<Layer>, int> _layerTransparentIndexMap;

        // A mapping of Layer to its opaque index in layerList.
        std::map<std::shared_ptr<Layer>, int> _layerOpaqueIndexMap;

        // A mapping of Layer::id to Layer.
        std::map<int, std::shared_ptr<Layer>> _layerIdMap;

        // A mapping of Layer::name to Layer.
        std::map<std::string, std::shared_ptr<Layer>> _layerNameMap;
    };
}
