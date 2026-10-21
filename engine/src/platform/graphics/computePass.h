// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "renderPass.h"

namespace visutwin::canvas
{
    class Compute;

    /**
     * A frame pass which dispatches a list of compute instances, in order, at its position in
     * the frame: after a camera renders (CameraComponent::afterPasses), say, to process the
     * scene maps it rendered in the same frame.
     *
     * Each compute is dispatched with the dispatch size and parameters set on it, typically
     * before the frame renders, or in the pass's onBefore. The pass has no render target, so it
     * starts no device pass and the frame graph merges neither of its neighbours across it.
     * Both backends order the dispatch after every pass before it and before every pass after
     * it (Metal encodes into its one open command buffer, Vulkan records into the frame's).
     *
     * The pass does not own the computes, which must outlive it or be removed from it.
     */
    class ComputePass : public RenderPass
    {
    public:
        explicit ComputePass(const std::shared_ptr<GraphicsDevice>& device, std::vector<Compute*> computes = {});

        /// The computes the pass dispatches, in order. Add or remove entries to change them.
        std::vector<Compute*>& computes() { return _computes; }
        const std::vector<Compute*>& computes() const { return _computes; }

        void setName(const std::string& name) { _name = name; }

        void execute() override;

    private:
        std::vector<Compute*> _computes;
    };
}
