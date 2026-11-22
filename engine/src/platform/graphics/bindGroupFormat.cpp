// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025
//
#include "bindGroupFormat.h"

#include <atomic>

namespace visutwin::canvas
{
    static std::atomic<uint32_t> idCounter{0};

    BindGroupFormat::BindGroupFormat(GraphicsDevice* /*graphicsDevice*/, const std::vector<BindBaseFormat*>& /*formats*/)
        : _id(idCounter.fetch_add(1, std::memory_order_relaxed)) {

    }
}