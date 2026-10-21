// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
#include "computePass.h"

#include <utility>

#include "graphicsDevice.h"

namespace visutwin::canvas
{
    ComputePass::ComputePass(const std::shared_ptr<GraphicsDevice>& device, std::vector<Compute*> computes)
        : RenderPass(device), _computes(std::move(computes))
    {
    }

    void ComputePass::execute()
    {
        if (!_computes.empty()) {
            device()->computeDispatch(_computes, name());
        }
    }
}
