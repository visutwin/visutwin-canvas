// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#include "layerComposition.h"

#include <spdlog/spdlog.h>
#include "framework/components/camera/cameraComponent.h"

namespace visutwin::canvas
{
    LayerComposition::~LayerComposition()
    {
        clearRenderActions();
    }

    bool LayerComposition::isSublayerAdded(const std::shared_ptr<Layer>& layer, bool transparent) const {
        if (const auto& map = transparent ? _layerTransparentIndexMap : _layerOpaqueIndexMap; map.contains(layer)) {
            spdlog::error("Sublayer {}, transparent: {} is already added.", layer->name(), transparent);
            return true;
        }
        return false;
    }

    void LayerComposition::pushOpaque(const std::shared_ptr<Layer>& layer) {
        // add opaque to the end of the array
        if (isSublayerAdded(layer, false))
        {
            return;
        }
        _layerList.push_back(layer);
        _opaqueOrder[layer->id()] = _subLayerList.size();
        _subLayerList.push_back(false);
        _subLayerEnabled.push_back(true);

        updateLayerMaps();
        _dirty = true;
        fire("add", layer);
    }

    void LayerComposition::pushTransparent(const std::shared_ptr<Layer>& layer) {
        // add transparent to the end of the array
        if (isSublayerAdded(layer, true))
        {
            return;
        }
        _layerList.push_back(layer);
        _transparentOrder[layer->id()] = _subLayerList.size();
        _subLayerList.push_back(true);
        _subLayerEnabled.push_back(true);

        updateLayerMaps();
        _dirty = true;
        fire("add", layer);
    }

    const std::vector<RenderAction*>& LayerComposition::renderActions()
    {
        const auto& cameras = CameraComponent::instances();
        if (cameras.size() != _lastCameraCount) {
            _dirty = true;
        }
        if (_renderActions.empty() && !cameras.empty()) {
            _dirty = true;
        }
        rebuildRenderActions();
        return _renderActions;
    }

    std::shared_ptr<Layer> LayerComposition::getLayerById(const int layerId) const
    {
        const auto it = _layerIdMap.find(layerId);
        return it != _layerIdMap.end() ? it->second : nullptr;
    }

    std::shared_ptr<Layer> LayerComposition::getLayerByName(const std::string& name) const
    {
        const auto it = _layerNameMap.find(name);
        return it != _layerNameMap.end() ? it->second : nullptr;
    }

    int LayerComposition::getOpaqueIndex(const std::shared_ptr<Layer>& layer) const
    {
        const auto it = _layerOpaqueIndexMap.find(layer);
        return it != _layerOpaqueIndexMap.end() ? it->second : -1;
    }

    int LayerComposition::getTransparentIndex(const std::shared_ptr<Layer>& layer) const
    {
        const auto it = _layerTransparentIndexMap.find(layer);
        return it != _layerTransparentIndexMap.end() ? it->second : -1;
    }

    bool LayerComposition::isEnabled(const Layer* layer, const bool transparent) const
    {
        if (!layer || !layer->enabled()) {
            return false;
        }

        const auto layerIt = _layerIdMap.find(layer->id());
        if (layerIt == _layerIdMap.end()) {
            return false;
        }

        const int index = transparent ? getTransparentIndex(layerIt->second) : getOpaqueIndex(layerIt->second);
        if (index < 0 || static_cast<size_t>(index) >= _subLayerEnabled.size()) {
            return false;
        }

        return _subLayerEnabled[index];
    }

    void LayerComposition::updateLayerMaps() {
        _layerIdMap.clear();
        _layerNameMap.clear();
        _layerOpaqueIndexMap.clear();
        _layerTransparentIndexMap.clear();

        for (size_t i = 0; i < _layerList.size(); i++) {
            auto& layer = _layerList[i];
            _layerIdMap[layer->id()] = layer;
            _layerNameMap[layer->name()] = layer;

            auto& subLayerIndexMap = _subLayerList[i] ? _layerTransparentIndexMap : _layerOpaqueIndexMap;
            subLayerIndexMap[layer] = i;
        }
    }

    void LayerComposition::clearRenderActions()
    {
        for (auto* action : _renderActions) {
            delete action;
        }
        _renderActions.clear();
    }

    void LayerComposition::rebuildRenderActions()
    {
        if (!_dirty) {
            for (const auto& layer : _layerList) {
                if (layer && layer->dirtyComposition()) {
                    _dirty = true;
                    break;
                }
            }
        }

        if (!_dirty) {
            return;
        }

        clearRenderActions();

        const auto& cameras = CameraComponent::instances();
        if (cameras.empty()) {
            _lastCameraCount = 0;
            _dirty = false;
            return;
        }

        for (auto* cameraComponent : cameras) {
            if (!cameraComponent || !cameraComponent->enabled() || !cameraComponent->camera()) {
                continue;
            }

            const bool useCameraPasses = !cameraComponent->renderPasses().empty();
            if (useCameraPasses) {
                auto* action = new RenderAction();
                action->camera = cameraComponent;
                action->useCameraPasses = true;
                _renderActions.push_back(action);
                continue;
            }

            bool firstCameraUse = true;
            RenderAction* lastRenderAction = nullptr;
            for (size_t i = 0; i < _layerList.size(); ++i) {
                if (i >= _subLayerEnabled.size() || !_subLayerEnabled[i]) {
                    continue;
                }

                const auto& layerRef = _layerList[i];
                if (!layerRef || !layerRef->enabled() || !cameraComponent->rendersLayer(layerRef->id())) {
                    continue;
                }

                auto* action = new RenderAction();
                action->camera = cameraComponent;
                action->useCameraPasses = useCameraPasses;
                action->layer = layerRef.get();
                action->renderTarget = (layerRef->id() == LAYERID_DEPTH)
                    ? nullptr
                    : cameraComponent->camera()->renderTarget();
                action->firstCameraUse = firstCameraUse;
                action->triggerPostprocess = false;
                action->transparent = _subLayerList[i];
                action->lastCameraUse = false;

                // Match upstream clear behavior: camera clears on first use / first use of target, layer clears always apply.
                bool usedCameraTarget = false;
                for (auto existingIt = _renderActions.rbegin(); existingIt != _renderActions.rend(); ++existingIt) {
                    const auto* existing = *existingIt;
                    if (!existing || existing->camera != cameraComponent) {
                        continue;
                    }
                    if (existing->renderTarget == action->renderTarget) {
                        usedCameraTarget = true;
                        break;
                    }
                }
                const bool needsCameraClear = firstCameraUse || !usedCameraTarget;
                if (needsCameraClear || layerRef->clearColorBuffer() || layerRef->clearDepthBuffer() || layerRef->clearStencilBuffer()) {
                    action->setupClears(needsCameraClear ? cameraComponent : nullptr, layerRef.get());
                }

                action->lastCameraUse = false;

                firstCameraUse = false;
                lastRenderAction = action;
                _renderActions.push_back(action);
            }

            if (lastRenderAction) {
                lastRenderAction->lastCameraUse = true;
                // DEVIATION: disablePostEffectsLayer / full camera stack propagation is not ported yet.
                // Keep parity for default behavior by triggering postprocess on the camera's last render action.
                lastRenderAction->triggerPostprocess = true;
            }
        }

        for (const auto& layer : _layerList) {
            if (layer) {
                layer->setDirtyComposition(false);
            }
        }
        _lastCameraCount = cameras.size();
        _dirty = false;
    }
}
