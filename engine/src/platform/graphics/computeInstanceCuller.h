// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The InstanceCuller every backend uses: two kernels (the Slang programs instance-cull-reset and instance-cull) dispatched
// through Compute and GraphicsDevice::computeDispatch, over buffers that are ordinary
// VertexBuffers. A backend gets GPU culling by supporting compute and indirect draws; it
// writes no culling code of its own.
//
#pragma once

#include <memory>

#include "platform/graphics/instanceCuller.h"

namespace visutwin::canvas
{
    class Compute;
    class GraphicsDevice;
    class Shader;

    class ComputeInstanceCuller final : public InstanceCuller
    {
    public:
        explicit ComputeInstanceCuller(GraphicsDevice* device);
        ~ComputeInstanceCuller() override;

        void reserve(uint32_t maxInstances) override;
        void cull(const std::shared_ptr<VertexBuffer>& input, const InstanceCullParams& params) override;
        [[nodiscard]] void* compactedNativeBuffer() const override;
        [[nodiscard]] void* indirectArgsNativeBuffer() const override;
        [[nodiscard]] uint32_t maxInstances() const override { return _maxInstances; }
        [[nodiscard]] uint32_t visibleCountReadback() const override;

    private:
        bool ensureKernels();

        GraphicsDevice* _device;
        std::shared_ptr<Shader> _resetShader;
        std::shared_ptr<Shader> _cullShader;
        std::unique_ptr<Compute> _reset;
        std::unique_ptr<Compute> _cull;
        std::shared_ptr<VertexBuffer> _args;     // 20-byte indexed-indirect arguments
        std::shared_ptr<VertexBuffer> _output;   // the visible instances, compacted
        uint32_t _maxInstances = 0;
        bool _unavailable = false;
    };
}
