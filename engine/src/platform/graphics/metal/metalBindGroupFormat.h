// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 14.11.2025.
//
#pragma once

#include "../bindGroupFormat.h"
#include "metalPipelineLayout.h"

namespace visutwin::canvas
{
    /**
     * Metal bind group format.
     * Describes the layout of buffers and textures in a bind group.
     */
    class MetalBindGroupFormat : public BindGroupFormat
    {
    public:
        MetalBindGroupFormat(GraphicsDevice* graphicsDevice, const std::vector<BindBaseFormat*>& formats):
            BindGroupFormat(graphicsDevice, formats) {}

        metal::BindGroupLayout* bindGroupLayout() const { return _bindGroupLayout; }

    private:
        metal::BindGroupLayout* _bindGroupLayout;
    };
}
