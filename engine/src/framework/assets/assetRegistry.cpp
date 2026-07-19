// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#include "assetRegistry.h"

#include <algorithm>
#include <stdexcept>

#include "asset.h"

namespace visutwin::canvas
{
    AssetRegistry::AssetRegistry(std::shared_ptr<ResourceLoader> resourceLoader)
        : _resourceLoader(std::move(resourceLoader))
    {
        if (!_resourceLoader) {
            throw std::invalid_argument("AssetRegistry requires a ResourceLoader");
        }
    }

    AssetRegistry::~AssetRegistry() = default;

    Asset* AssetRegistry::add(std::unique_ptr<Asset> asset)
    {
        if (!asset) {
            throw std::invalid_argument("Cannot register a null Asset");
        }

        const auto name = asset->name();
        if (_assets.contains(name)) {
            throw std::invalid_argument("An Asset named '" + name + "' is already registered");
        }

        auto* observer = asset.get();
        _assets.emplace(name, std::move(asset));
        fire("add", observer);
        return observer;
    }

    std::unique_ptr<Asset> AssetRegistry::remove(Asset* asset)
    {
        if (!asset) {
            return nullptr;
        }

        const auto it = std::find_if(_assets.begin(), _assets.end(), [asset](const auto& entry) {
            return entry.second.get() == asset;
        });
        if (it == _assets.end()) {
            return nullptr;
        }

        auto ownership = std::move(it->second);
        _assets.erase(it);
        fire("remove", asset);
        return ownership;
    }

    Asset* AssetRegistry::findByName(const std::string& name) const
    {
        const auto it = _assets.find(name);
        return it != _assets.end() ? it->second.get() : nullptr;
    }

    std::vector<Asset*> AssetRegistry::list(bool* preloadFilter) const
    {
        std::vector<Asset*> assets;
        assets.reserve(_assets.size());
        for (const auto& [_, asset] : _assets) {
            assets.push_back(asset.get());
        }

        if (preloadFilter != nullptr) {
            std::vector<Asset*> filtered;
            for (auto* asset : assets) {
                // Check asset preload property - assuming Asset has getPreload() method
                if (asset->preload() == *preloadFilter) {
                    filtered.push_back(asset);
                }
            }
            return filtered;
        }

        return assets;
    }
}
