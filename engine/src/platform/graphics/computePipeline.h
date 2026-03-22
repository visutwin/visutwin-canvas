// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Base interface for compute pipeline state caching.
// Backend implementations (Metal, Vulkan) provide concrete PSO management.
//
#pragma once

namespace visutwin::canvas
{
    /**
     * Abstract base for compute pipeline state objects.
     * Backends subclass this to manage their own pipeline caching.
     */
    class ComputePipelineBase
    {
    public:
        virtual ~ComputePipelineBase() = default;
    };
}
