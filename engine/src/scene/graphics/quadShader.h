// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The device-cached shader of a fullscreen pass or a bake: looked up by name, and on the
// first request created from the source in the device's language and cached. Every quad
// effect and offline bake gets its shader through here, so they agree on the cache key,
// on what a miss costs and on what a failure leaves behind (nothing: a shader that failed
// to compile is not cached, and the next request tries again).
//
#pragma once

#include <memory>
#include <string>
#include <utility>

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class Shader;

    /// Creates the shader `cacheKey` from `source` and caches it on the device. What a
    /// miss of getOrCreateQuadShader calls; returns null (and caches nothing) on failure.
    std::shared_ptr<Shader> createCachedQuadShader(GraphicsDevice* device, const char* cacheKey,
        const char* vertexEntry, const char* fragmentEntry, const std::string& source);

    /// The cached shader, building its source only on a miss: `buildSource(bool glsl)`
    /// returns the source for the device's language. For sources assembled per call
    /// (a define in front, a template filled in), which a hit must not pay for.
    template <typename BuildSource>
    std::shared_ptr<Shader> getOrCreateQuadShader(GraphicsDevice* device, const char* cacheKey,
        const char* vertexEntry, const char* fragmentEntry, BuildSource&& buildSource)
    {
        if (!device) {
            return nullptr;
        }
        if (auto cached = device->getCachedShader(cacheKey)) {
            return cached;
        }
        const bool glsl = device->shaderLanguage() == ShaderLanguage::Glsl;
        return createCachedQuadShader(device, cacheKey, vertexEntry, fragmentEntry,
            std::forward<BuildSource>(buildSource)(glsl));
    }

    /// The cached shader from a fixed MSL and GLSL source.
    inline std::shared_ptr<Shader> getOrCreateQuadShader(GraphicsDevice* device, const char* cacheKey,
        const char* vertexEntry, const char* fragmentEntry, const char* msl, const char* glsl)
    {
        return getOrCreateQuadShader(device, cacheKey, vertexEntry, fragmentEntry,
            [&](const bool isGlsl) { return std::string(isGlsl ? glsl : msl); });
    }
}
