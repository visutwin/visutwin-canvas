// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2025.
//
#pragma once

#include <vector>

#include "metalBuffer.h"
#include "platform/graphics/indexBuffer.h"

namespace visutwin::canvas
{
    class MetalIndexBuffer : public IndexBuffer, public gpu::MetalBuffer
    {
    public:
        MetalIndexBuffer(GraphicsDevice* graphicsDevice, IndexFormat format, int numIndices);

        bool setData(const std::vector<uint8_t>& data) override;

        [[nodiscard]] MTL::Buffer* raw() const { return gpu::MetalBuffer::raw(); }

        void* nativeBuffer() const override { return raw(); }

    };
}
