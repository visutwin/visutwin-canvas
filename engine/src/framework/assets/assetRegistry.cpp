// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#include "assetRegistry.h"
#include "asset.h"

namespace visutwin::canvas
{
    AssetRegistry::AssetRegistry(const std::shared_ptr<ResourceLoader>& resourceLoader) {}

    std::vector<Asset*> AssetRegistry::list(bool* preloadFilter) const
    {
        std::vector assets(_assets.begin(), _assets.end());

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