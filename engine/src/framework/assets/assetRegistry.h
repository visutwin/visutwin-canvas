// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
        explicit AssetRegistry(std::shared_ptr<ResourceLoader> resourceLoader);
        ~AssetRegistry() override;

        AssetRegistry(const AssetRegistry&) = delete;
        AssetRegistry& operator=(const AssetRegistry&) = delete;

        /** Transfer ownership of an asset into the registry. Names must be unique. */
        Asset* add(std::unique_ptr<Asset> asset);

        /** Remove an asset and transfer ownership back to the caller. */
        [[nodiscard]] std::unique_ptr<Asset> remove(Asset* asset);

        Asset* findByName(const std::string& name) const;

        // Create a filtered list of assets from the registry
        std::vector<Asset*> list(bool* preloadFilter = nullptr) const;

        const std::shared_ptr<ResourceLoader>& loader() const { return _resourceLoader; }

    private:
        std::shared_ptr<ResourceLoader> _resourceLoader;
        std::unordered_map<std::string, std::unique_ptr<Asset>> _assets;
    };
}
