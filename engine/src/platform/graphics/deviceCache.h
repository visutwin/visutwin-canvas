// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once

#include <functional>
#include <memory>

#include "graphicsDevice.h"

namespace visutwin::canvas
{
    /**
     * A cache storing shared resources associated with a device. The resources are removed
     * from the cache when the device is destroyed.
     */
    class DeviceCache
    {
    public:
        void remove(const std::shared_ptr<GraphicsDevice>& _graphicsDevice);

        template <class T>
        std::shared_ptr<T> get(const std::shared_ptr<GraphicsDevice>& device,
                               std::function<std::shared_ptr<T>()> onCreate);

    private:
        void handleDeviceLost(GraphicsDevice* device);

        std::unordered_map<GraphicsDevice*, std::shared_ptr<void>> _cache;
    };

    template<typename T>
    std::shared_ptr<T> DeviceCache::get(const std::shared_ptr<GraphicsDevice>& device, std::function<std::shared_ptr<T>()> onCreate) {
        auto it = _cache.find(device.get());

        if (it == _cache.end()) {
            // Create the resource
            auto resource = onCreate();

            // Store in cache (casting to void* for storage)
            _cache[device.get()] = std::static_pointer_cast<void>(resource);

            // Set up event handlers for the device lifecycle
            auto destroyCallback = [this, device]() {
                remove(device);
            };

            auto deviceLostCallback = [this, device]() {
                handleDeviceLost(device.get());
            };

            // Register event handlers
            device->on("destroy", destroyCallback);
            device->on("devicelost", deviceLostCallback);

            return resource;
        }

        // Return cached resource (casting back from void*)
        return std::static_pointer_cast<T>(_cache[device.get()]);
    }
}
