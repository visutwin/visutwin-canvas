// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once

#include <unordered_set>

#include "core/eventHandler.h"
#include "framework/handlers/resourceLoader.h"

namespace visutwin::canvas
{
    class Asset;

    /**
     * Container for all assets that are available to this application. Note that scripts
     * are provided with an AssetRegistry instance as `app.assets`.
     */
    class AssetRegistry : public EventHandler
    {
    public:
        AssetRegistry(const std::shared_ptr<ResourceLoader>& resourceLoader);

        // Create a filtered list of assets from the registry
        std::vector<Asset*> list(bool* preloadFilter = nullptr) const;

    private:
        std::unordered_set<Asset*> _assets;
    };
}
