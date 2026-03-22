// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.11.2025.
//

#include "metalComputePipeline.h"

#include "metalShader.h"
#include "platform/graphics/shader.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    MetalComputePipeline::~MetalComputePipeline()
    {
        for (const auto& [_, pipeline] : _cache) {
            if (pipeline) {
                pipeline->release();
            }
        }
        _cache.clear();
    }

    MTL::ComputePipelineState* MetalComputePipeline::get(const std::shared_ptr<Shader>& shader)
    {
        if (!shader) {
            return nullptr;
        }

        const auto it = _cache.find(shader->id());
        if (it != _cache.end()) {
            return it->second;
        }

        auto* pipeline = create(shader);
        if (pipeline) {
            _cache.emplace(shader->id(), pipeline);
        }
        return pipeline;
    }

    MTL::ComputePipelineState* MetalComputePipeline::create(const std::shared_ptr<Shader>& shader)
    {
        if (!shader || !_device) {
            return nullptr;
        }

        auto* metalDevice = const_cast<MTL::Device*>(_device->raw());
        if (!metalDevice) {
            return nullptr;
        }

        auto* metalShader = dynamic_cast<MetalShader*>(shader.get());
        if (!metalShader) {
            spdlog::error("Unsupported shader implementation for Metal compute pipeline. Expected MetalShader.");
            return nullptr;
        }

        NS::Error* error = nullptr;
        const auto* bundle = NS::Bundle::mainBundle();
        auto* library = metalShader->getLibrary(metalDevice, bundle, &error);
        if (!library) {
            spdlog::error("Failed to get Metal library for compute shader: {}",
                error ? error->localizedDescription()->utf8String() : "unknown");
            return nullptr;
        }

        library->retain();
        const auto& computeEntry = shader->computeEntry().empty() ? std::string("computeMain") : shader->computeEntry();
        auto* computeFunction = library->newFunction(NS::String::string(computeEntry.c_str(), NS::UTF8StringEncoding));
        if (!computeFunction) {
            spdlog::error("Failed to find compute shader entry point '{}'", computeEntry);
            library->release();
            return nullptr;
        }

        auto* pipeline = metalDevice->newComputePipelineState(computeFunction, &error);
        if (!pipeline) {
            spdlog::error("Failed to create compute pipeline state: {}",
                error ? error->localizedDescription()->utf8String() : "unknown");
        }

        computeFunction->release();
        library->release();
        return pipeline;
    }
} // visutwin
