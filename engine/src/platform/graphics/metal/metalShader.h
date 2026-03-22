// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2025.
//
#pragma once

#include <unordered_map>
#include <Metal/Metal.hpp>
#include <Foundation/NSBundle.hpp>

#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    /**
     * Metal shader implementation.
     * Manages MTL::Library and MTL::Function objects for vertex and fragment shaders.
     */
    class MetalShader : public Shader
    {
    public:
        MetalShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition, std::string sourceCode = "");
        ~MetalShader() override;

        MTL::Library* getLibrary(MTL::Device* device, const NS::Bundle* bundle, NS::Error** error);

    private:
        std::string _sourceCode;
        std::unordered_map<MTL::Device*, MTL::Library*> _libraries;
    };
}
