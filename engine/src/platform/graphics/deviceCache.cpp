// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#include "deviceCache.h"

namespace visutwin::canvas
{
    void DeviceCache::remove(const std::shared_ptr<GraphicsDevice>& device) {
        auto it = _cache.find(device.get());
        if (it != _cache.end()) {
            // Get the cached resource
            auto resource = it->second;

            // Remove from cache
            _cache.erase(it);
        }
    }

    void DeviceCache::handleDeviceLost(GraphicsDevice* device)
    {
        auto it = _cache.find(device);
        if (it != _cache.end()) {
            // Get the cached resource
            auto resource = it->second;

            // Try to call loseContext method if the resource has one
            // Note: Similar to destroy(), this requires the concrete type
            // or a common interface. The JavaScript version uses optional
            // chaining: resource?.loseContext?.(device)
            //
            // In a full C++ implementation, you would either:
            // 1. Define an interface with loseContext() method
            // 2. Use type traits to detect if the type has loseContext()
            // 3. Let the resource handle device loss through the event system
        }
    }
}