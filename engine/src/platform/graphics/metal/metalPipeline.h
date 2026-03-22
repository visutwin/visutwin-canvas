// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers  on 05.11.2025.
//
#pragma once

#include <memory>
#include <vector>

#include "metalGraphicsDevice.h"
#include "platform/graphics/bindGroupFormat.h"
#include "metalPipelineLayout.h"

namespace visutwin::canvas
{
    // Base class for render and compute pipelines
    class MetalPipeline
    {
    public:
        explicit MetalPipeline(const MetalGraphicsDevice* device): _device(device) {}

        /**
         * Get or create a pipeline layout for the given bind group formats.
         */
        metal::PipelineLayout* getPipelineLayout(const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats);

    protected:
        const MetalGraphicsDevice* _device;
    };
}
