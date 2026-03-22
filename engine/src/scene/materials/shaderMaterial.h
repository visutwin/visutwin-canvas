// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026.
//
#pragma once

#include <memory>
#include <string>

#include "material.h"

namespace visutwin::canvas
{
    class GraphicsDevice;

    class ShaderMaterial : public Material
    {
    public:
        ShaderMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::string& uniqueName,
            const std::string& vertexEntry = "vertexShader", const std::string& fragmentEntry = "fragmentShader",
            const std::string& sourceCode = "");
    };
}
