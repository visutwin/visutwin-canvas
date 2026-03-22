// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.11.2025.
//
#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "platform/graphics/vertexFormat.h"

namespace visutwin::canvas
{
    /**
     * Metal vertex buffer layout management class.
     * Handles creation and caching of vertex buffer layouts for rendering pipelines.
     *
     * This class converts VertexFormat objects into Metal-compatible vertex buffer layouts,
     * handling both interleaved and non-interleaved vertex data configurations.
     */
    class MetalVertexBufferLayout
    {
    public:
        /**
         * Get a vertex layout for one or two vertex formats. Results are cached for performance.
         */
        std::vector<void*> get(const std::shared_ptr<VertexFormat>& vertexFormat0,
            const std::shared_ptr<VertexFormat>& vertexFormat1 = nullptr);

    private:
        /**
         * Generate a cache key for a pair of vertex formats.
         */
        std::string getKey(const std::shared_ptr<VertexFormat>& vertexFormat0,
            const  std::shared_ptr<VertexFormat>& vertexFormat1) const;

        /**
         * Create a vertex buffer layout for one or two vertex formats.
         * Note: If the VertexFormat is interleaved, we use a single vertex buffer with multiple
         * attributes. This uses a smaller number of vertex buffers (1), which has performance
         * benefits when setting it up on the device.
         * If the VertexFormat is not interleaved, we use multiple vertex buffers, one per
         * attribute. This is less efficient but is required as there is a pretty small
         * limit on the attribute offsets in the vertex buffer layout.
         */
        std::vector<void*> create(const std::shared_ptr<VertexFormat>& vertexFormat0,
            const std::shared_ptr<VertexFormat>& vertexFormat1 = nullptr);

        // Cache of vertex buffer layouts keyed by a format combination
        std::unordered_map<std::string, std::vector<void*>> _cache;
    };
}
