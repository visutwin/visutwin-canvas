// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2025
//
#include "metalPipeline.h"

#include <atomic>

#include <spdlog/spdlog.h>

#include "metalBindGroupFormat.h"

namespace visutwin::canvas
{
    static std::atomic<int> nextLayoutId{0};

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

        const int layoutId = nextLayoutId.fetch_add(1, std::memory_order_relaxed) + 1;

        auto pipelineLayout = new metal::PipelineLayout(desc);
        pipelineLayout->setDebugLabel(layoutId, "PipelineLayout-" + std::to_string(layoutId));

        spdlog::trace("Alloc MetalPipeline: Id " + std::to_string(layoutId));

        return pipelineLayout;
    }
}
