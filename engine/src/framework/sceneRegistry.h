// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once

#include <memory>

namespace visutwin::canvas
{
    class Engine;

    /**
     * Container for storing and loading of scenes. An instance of the registry is created on the
     * {@link Engine} object.
     */
    class SceneRegistry
    {
    public:
        SceneRegistry(const std::shared_ptr<Engine>& engine) {}
    };
}
