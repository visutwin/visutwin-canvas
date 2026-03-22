// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026.
//
#include "shaderMaterial.h"

#include "platform/graphics/graphicsDevice.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    ShaderMaterial::ShaderMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::string& uniqueName,
        const std::string& vertexEntry, const std::string& fragmentEntry, const std::string& sourceCode)
    {
        setName(uniqueName);
        setTransparent(false);

        if (device) {
            if (sourceCode.empty()) {
                spdlog::warn("ShaderMaterial '{}' created without source code. Shader override was not created.", uniqueName);
                return;
            }
            ShaderDefinition definition;
            definition.name = uniqueName;
            definition.vshader = vertexEntry;
            definition.fshader = fragmentEntry;
            setShaderOverride(createShader(device.get(), definition, sourceCode));
        }
    }
}
