// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 20.12.2025.
//
#pragma once
#include <framework/entity.h>

namespace visutwin::canvas
{
    /**
     * Container for a list of animations, textures, materials, renders and a model
     */
    class ContainerResource
    {
    public:
        virtual ~ContainerResource() = default;

        virtual Entity* instantiateRenderEntity() = 0;
    };
}
