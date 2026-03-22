// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025.
//
#include "bindGroupFormat.h"

namespace visutwin::canvas
{
    static uint32_t idCounter = 0;

    BindGroupFormat::BindGroupFormat(GraphicsDevice* graphicsDevice, const std::vector<BindBaseFormat*>& formats)
        : _device(graphicsDevice), _id(idCounter++) {

    }
}