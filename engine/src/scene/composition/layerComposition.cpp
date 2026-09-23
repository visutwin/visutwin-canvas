// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#include "layerComposition.h"

#include <algorithm>

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
        _subLayerList.push_back(true);
        _subLayerEnabled.push_back(true);

        updateLayerMaps();
        _dirty = true;
        fire("add", layer);
    }

    void LayerComposition::insertOpaque(const std::shared_ptr<Layer>& layer, const int index)
    {
        if (!layer || isSublayerAdded(layer, false)) {
            return;
        }
        const int clamped = std::clamp(index, 0, static_cast<int>(_layerList.size()));
        _layerList.insert(_layerList.begin() + clamped, layer);
        _subLayerList.insert(_subLayerList.begin() + clamped, false);
        _subLayerEnabled.insert(_subLayerEnabled.begin() + clamped, true);

        updateLayerMaps();
        _dirty = true;
        fire("add", layer);
    }

    void LayerComposition::remove(const std::shared_ptr<Layer>& layer)
    {
        if (!layer) {
            return;
        }
        bool removed = false;
        for (int i = static_cast<int>(_layerList.size()) - 1; i >= 0; --i) {
            if (_layerList[i] == layer) {
                _layerList.erase(_layerList.begin() + i);
                _subLayerList.erase(_subLayerList.begin() + i);
                _subLayerEnabled.erase(_subLayerEnabled.begin() + i);
                removed = true;
            }
        }
        if (removed) {
            updateLayerMaps();
            _dirty = true;
            fire("remove", layer);
        }
    }

    void LayerComposition::insert(const std::shared_ptr<Layer>& layer, int index)
    {
        if (!layer) {
            return;
        }
        if (isSublayerAdded(layer, false) || isSublayerAdded(layer, true)) {
            return;
        }
        // Mirrors upstream LayerComposition::insert: BOTH sublayers of the layer go in
        // at the index, opaque first then transparent, so the layer as a whole slots
        // into the existing order (e.g. at another layer's transparent index, which
        // puts it after that layer's opaque meshes but before its transparent ones).
        const int clamped = std::clamp(index, 0, static_cast<int>(_layerList.size()));

        _layerList.insert(_layerList.begin() + clamped, {layer, layer});
        _subLayerList.insert(_subLayerList.begin() + clamped, {false, true});
        _subLayerEnabled.insert(_subLayerEnabled.begin() + clamped, {true, true});

        updateLayerMaps();
        _dirty = true;
        fire("add", layer);
    }

    const std::vector<RenderAction*>& LayerComposition::renderActions()
    {
        const auto& cameras = CameraComponent::instances();
        // Fingerprint the camera state the render actions bake in: identity,
        // active state, render target, camera-passes mode, layer list and the
        // clear flags (setupClears copies them into the actions).
        // A count-only comparison missed enabled toggles, render-target changes,
        // and destroy+create at equal count — all of which left stale actions
        // holding dangling RenderAction::camera pointers.
        size_t fingerprint = cameras.size();
        const auto mix = [&fingerprint](size_t v) {
            fingerprint ^= v + 0x9e3779b97f4a7c15ull + (fingerprint << 6) + (fingerprint >> 2);
        };
        for (const auto* cameraComponent : cameras) {
            mix(reinterpret_cast<size_t>(cameraComponent));
            if (cameraComponent) {
                // active(), not enabled(): a camera on a disabled entity renders
                // nothing, and switching the ENTITY must rebuild the actions just as
                // switching the component does.
                mix(cameraComponent->active() ? 1u : 2u);
                const auto* camera = cameraComponent->camera();
                mix(reinterpret_cast<size_t>(camera ? camera->renderTarget().get() : nullptr));
                if (camera) {
                    mix((camera->clearColorBufferEnabled() ? 1u : 0u) |
                        (camera->clearDepthBufferEnabled() ? 2u : 0u) |
                        (camera->clearStencilBufferEnabled() ? 4u : 0u));
                }
                mix(cameraComponent->renderPasses().size());
                // The render actions bake in the camera ORDER, so a priority change
                // has to rebuild them like any other identity change.
                mix(static_cast<size_t>(cameraComponent->priority()));
                for (const int layerId : cameraComponent->layers()) {
                    mix(static_cast<size_t>(layerId));
                }
            }
        }
        if (fingerprint != _lastCameraFingerprint) {
            _lastCameraFingerprint = fingerprint;
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
        // Rebuilt from the sublayer list rather than maintained incrementally —
        // insert() shifts every index after it, which stale entries would miss.
        _opaqueOrder.clear();
        _transparentOrder.clear();

        for (size_t i = 0; i < _layerList.size(); i++) {
            auto& layer = _layerList[i];
            _layerIdMap[layer->id()] = layer;
            _layerNameMap[layer->name()] = layer;

            auto& subLayerIndexMap = _subLayerList[i] ? _layerTransparentIndexMap : _layerOpaqueIndexMap;
            subLayerIndexMap[layer] = i;

            auto& subLayerOrder = _subLayerList[i] ? _transparentOrder : _opaqueOrder;
            subLayerOrder[layer->id()] = static_cast<int>(i);
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
            _dirty = false;
            return;
        }

        // Cameras render in PRIORITY order, smallest first (upstream). The sort is
        // stable and everything defaults to 0, so a scene that sets no priority keeps
        // the construction order it always had — which is what a dynamic reflection
        // probe used to depend on, and now does not have to.
        std::vector<CameraComponent*> ordered(cameras.begin(), cameras.end());
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const CameraComponent* a, const CameraComponent* b) {
                return (a ? a->priority() : 0) < (b ? b->priority() : 0);
            });

        for (auto* cameraComponent : ordered) {
            if (!cameraComponent || !cameraComponent->active() || !cameraComponent->camera()) {
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
        _dirty = false;
    }
}
