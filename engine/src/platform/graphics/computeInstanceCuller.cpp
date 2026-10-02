// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "computeInstanceCuller.h"

#include <spdlog/spdlog.h>

#include "platform/graphics/compute.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/instanceCullShaders.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr int kInstanceSize = 80;    // InstanceData: model matrix + colour
        constexpr int kDrawArgsSize = 20;    // indexed-indirect arguments

        // One shader per kernel per device, shared by every culler.
        std::shared_ptr<Shader> cullKernel(GraphicsDevice* device, const char* cacheKey, const char* entry,
            const char* glsl)
        {
            if (auto cached = device->getCachedShader(cacheKey)) {
                return cached;
            }
            ShaderDefinition definition;
            definition.name = cacheKey;
            definition.cshader = entry;
            auto shader = createShader(device, definition,
                device->shaderLanguage() == ShaderLanguage::Glsl ? glsl : instance_cull_shaders::INSTANCE_CULL_MSL);
            if (shader) {
                device->setCachedShader(cacheKey, shader);
            }
            return shader;
        }

        std::shared_ptr<VertexBuffer> zeroedBuffer(GraphicsDevice* device, const int stride, const int count)
        {
            auto format = std::make_shared<VertexFormat>(stride, true, false);
            VertexBufferOptions options;
            options.data.assign(static_cast<size_t>(stride) * static_cast<size_t>(count), 0);
            return device->createVertexBuffer(format, count, options);
        }
    }

    ComputeInstanceCuller::ComputeInstanceCuller(GraphicsDevice* device) : _device(device) {}

    ComputeInstanceCuller::~ComputeInstanceCuller() = default;

    bool ComputeInstanceCuller::ensureKernels()
    {
        if (_reset && _cull) {
            return true;
        }
        if (_unavailable || !_device || !_device->supportsCompute()) {
            _unavailable = true;
            return false;
        }
        _resetShader = cullKernel(_device, "instance-cull-reset", "instanceCullReset",
            instance_cull_shaders::INSTANCE_CULL_RESET_GLSL);
        _cullShader = cullKernel(_device, "instance-cull", "instanceCull", instance_cull_shaders::INSTANCE_CULL_GLSL);
        _args = zeroedBuffer(_device, kDrawArgsSize, 1);
        if (!_resetShader || !_cullShader || !_args) {
            spdlog::error("[InstanceCuller] GPU culling unavailable: kernels or buffers could not be created");
            _unavailable = true;
            return false;
        }
        _reset = std::make_unique<Compute>(_device, _resetShader, "InstanceCullReset");
        _reset->setParameter("args", _args);
        _reset->setThreadgroupSize(1u, 1u, 1u);
        _reset->setupDispatch(1u, 1u, 1u);
        _cull = std::make_unique<Compute>(_device, _cullShader, "InstanceCull");
        _cull->setParameter("args", _args);
        _cull->setThreadgroupSize(instance_cull_shaders::kCullThreads, 1u, 1u);
        return true;
    }

    void ComputeInstanceCuller::reserve(const uint32_t maxInstances)
    {
        if (!ensureKernels() || (_output && maxInstances <= _maxInstances)) {
            return;
        }
        _output = zeroedBuffer(_device, kInstanceSize, static_cast<int>(std::max(maxInstances, 1u)));
        _maxInstances = _output ? std::max(maxInstances, 1u) : 0u;
        // The kernels bind the buffer by name, and a new allocation is a new buffer.
        _reset->setParameter("output", _output);
        _cull->setParameter("output", _output);
    }

    void ComputeInstanceCuller::cull(const std::shared_ptr<VertexBuffer>& input, const InstanceCullParams& params)
    {
        if (!input || params.instanceCount == 0) {
            return;
        }
        reserve(params.instanceCount);
        if (!_output) {
            return;
        }
        for (Compute* compute : {_reset.get(), _cull.get()}) {
            compute->setParameter("input", input);
            compute->setUniformBlock(&params, sizeof(params));
        }
        _cull->setupDispatch(
            (params.instanceCount + instance_cull_shaders::kCullThreads - 1u) / instance_cull_shaders::kCullThreads,
            1u, 1u);
        _device->computeDispatch({_reset.get(), _cull.get()}, "instance-cull");
    }

    void* ComputeInstanceCuller::compactedNativeBuffer() const
    {
        return _output ? _output->nativeBuffer() : nullptr;
    }

    void* ComputeInstanceCuller::indirectArgsNativeBuffer() const
    {
        return _args ? _args->nativeBuffer() : nullptr;
    }

    uint32_t ComputeInstanceCuller::visibleCountReadback() const
    {
        uint32_t count = 0;
        if (!_args || !_args->read(sizeof(uint32_t), sizeof(count), &count)) {
            return 0;
        }
        return count;
    }
}
