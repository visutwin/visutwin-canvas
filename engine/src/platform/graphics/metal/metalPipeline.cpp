// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2025.
//
#include "metalPipeline.h"

#include <spdlog/spdlog.h>

#include "metalBindGroupFormat.h"

namespace visutwin::canvas
{
    static int _layoutId = 0;

    metal::PipelineLayout* MetalPipeline::getPipelineLayout(const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats) {
        // Collect bind group layouts from formats
        std::vector<metal::BindGroupLayout*> bindGroupLayouts;
        bindGroupLayouts.reserve(bindGroupFormats.size());

        for (const auto& format : bindGroupFormats) {
            if (format) {
                bindGroupLayouts.push_back(std::static_pointer_cast<MetalBindGroupFormat>(format)->bindGroupLayout());
            }
        }

        metal::PipelineLayoutDesc desc { bindGroupLayouts };

        _layoutId++;

        auto pipelineLayout = new metal::PipelineLayout(desc);
        pipelineLayout->setDebugLabel(_layoutId, "PipelineLayout-" + std::to_string(_layoutId));

        spdlog::trace("Alloc MetalPipeline: Id " + std::to_string(_layoutId));

        return pipelineLayout;
    }
}
