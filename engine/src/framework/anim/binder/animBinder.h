// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace visutwin::canvas
{
    class GraphNode;
    class MorphInstance;

    class AnimBinder
    {
    public:
        virtual ~AnimBinder() = default;

        virtual GraphNode* resolve(const std::string& path) = 0;
        virtual void unresolve(const std::string& path) = 0;

        /// Changes whenever an answer resolve() or resolveMorphInstances() gave may no
        /// longer hold (an unresolve). The evaluator and AnimComponent cache those
        /// answers per node and look again only when this moves, so a binder whose
        /// answers can change must change it too.
        virtual uint64_t version() const { return 0; }

        /// Morph instances animated by a "weights" channel targeting the node at
        /// `path` (the glTF mesh node). Default: none.
        virtual std::vector<MorphInstance*> resolveMorphInstances(const std::string& path)
        {
            (void)path;
            return {};
        }
    };
}
