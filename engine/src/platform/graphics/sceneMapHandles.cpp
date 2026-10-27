// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 27.10.2026
//
#include "sceneMapHandles.h"

#include "texture.h"

namespace visutwin::canvas
{
    uint32_t SceneDepthMapHandle::width() const
    {
        const auto map = texture.lock();
        return map ? map->width() : 0u;
    }

    uint32_t SceneDepthMapHandle::height() const
    {
        const auto map = texture.lock();
        return map ? map->height() : 0u;
    }

    uint32_t SceneColorMapHandle::width() const
    {
        const auto map = texture.lock();
        return map ? map->width() : 0u;
    }

    uint32_t SceneColorMapHandle::height() const
    {
        const auto map = texture.lock();
        return map ? map->height() : 0u;
    }
}
