// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "quadShader.h"

#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    std::shared_ptr<Shader> createCachedQuadShader(GraphicsDevice* device, const char* cacheKey,
        const char* vertexEntry, const char* fragmentEntry, const std::string& source)
    {
        ShaderDefinition definition;
        definition.name = cacheKey;
        definition.vshader = vertexEntry;
        definition.fshader = fragmentEntry;
        auto shader = createShader(device, definition, source);
        if (shader) {
            device->setCachedShader(cacheKey, shader);
        }
        return shader;
    }
}
