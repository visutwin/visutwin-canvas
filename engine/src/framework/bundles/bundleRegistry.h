// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once

#include <memory>

#include "framework/assets/assetRegistry.h"

namespace visutwin::canvas
{
    /**
     * Keeps track of which assets are in bundles and loads files from bundles.
     */
    class BundleRegistry
    {
    public:
        BundleRegistry(const std::shared_ptr<AssetRegistry>& assetRegistry) {}
    };
}
