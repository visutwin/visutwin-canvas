// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.03.2026.
//
//
//
#pragma once

#include <string>
#include <vector>

namespace visutwin::canvas
{
    /**
     * Holds batch group settings: which mesh instances belong together, layer assignments,
     * dynamic flag, and maximum AABB size for spatial splitting.
     *
     * A batch group acts as a recipe — at prepare() time, BatchManager collects all
     * MeshInstances tagged with a group's id, groups them by material, and merges
     * each sub-group into a single draw call.
     */
    struct BatchGroup
    {
        /** Sentinel value: mesh instance does not belong to any batch group. */
        static constexpr int NOID = -1;

        int id = NOID;
        std::string name;

        /** When true, transforms may change per frame (uses SkinBatchInstance trick).
         *  Phase 1: always false — only static batching is supported. */
        bool dynamic = false;

        /** Maximum AABB dimension for spatial splitting. 0 = no limit. */
        float maxAabbSize = 0.0f;

        /** Layer IDs this batch group applies to. */
        std::vector<int> layers;

        BatchGroup() = default;

        BatchGroup(int id, const std::string& name, bool dynamic = false,
                   float maxAabbSize = 0.0f, const std::vector<int>& layers = {})
            : id(id), name(name), dynamic(dynamic), maxAabbSize(maxAabbSize), layers(layers) {}
    };
}
