// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2025.
//
#pragma once

#include <unordered_map>

#include "metalPipeline.h"
#include "platform/graphics/computePipeline.h"

namespace visutwin::canvas
{
    class Shader;

    // Metal compute pipeline cache and creation
    class MetalComputePipeline : public MetalPipeline, public ComputePipelineBase
    {
    public:
        explicit MetalComputePipeline(const MetalGraphicsDevice* device): MetalPipeline(device) {}
        ~MetalComputePipeline();

        MTL::ComputePipelineState* get(const std::shared_ptr<Shader>& shader);

    private:
        MTL::ComputePipelineState* create(const std::shared_ptr<Shader>& shader);

        std::unordered_map<int, MTL::ComputePipelineState*> _cache;
    };
}
