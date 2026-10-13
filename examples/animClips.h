// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// The animation examples' clip files: each bitmoji clip GLB holds one animation track,
// which binds to the character's rig by node path.
//
#pragma once

#include <memory>

#include <spdlog/spdlog.h>

#include "framework/anim/evaluator/animTrack.h"
#include "framework/assets/asset.h"
#include "framework/parsers/glbContainerResource.h"

namespace visutwin::canvas
{
    /// The first animation track of a container asset, or null (with an error logged).
    inline std::shared_ptr<AnimTrack> firstAnimTrack(Asset* asset)
    {
        auto* glb = asset ? dynamic_cast<GlbContainerResource*>(asset->resourceAs<ContainerResource>()) : nullptr;
        if (!glb || glb->animTracks().empty()) {
            spdlog::error("Animation '{}' holds no animation track", asset ? asset->name() : "?");
            return nullptr;
        }
        return glb->animTracks().begin()->second;
    }
}
