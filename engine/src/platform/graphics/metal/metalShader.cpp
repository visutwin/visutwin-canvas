// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2025.
//
#include "metalShader.h"

#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    MetalShader::MetalShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition, std::string sourceCode)
        : Shader(graphicsDevice, definition), _sourceCode(std::move(sourceCode))
    {
    }

    MetalShader::~MetalShader()
    {
        for (const auto& [_, library] : _libraries) {
            if (library) {
                library->release();
            }
        }
    }

    MTL::Library* MetalShader::getLibrary(MTL::Device* device, const NS::Bundle* bundle, NS::Error** error)
    {
        (void)bundle;
        if (!device) {
            return nullptr;
        }

        const auto found = _libraries.find(device);
        if (found != _libraries.end()) {
            return found->second;
        }

        if (_sourceCode.empty()) {
            spdlog::error("MetalShader source is empty. Source-less metallib fallback was removed.");
            return nullptr;
        }

        auto* compileOptions = MTL::CompileOptions::alloc()->init();
        compileOptions->setFastMathEnabled(true);
        MTL::Library* library = device->newLibrary(NS::String::string(_sourceCode.c_str(), NS::UTF8StringEncoding),
            compileOptions, error);
        compileOptions->release();

        if (library) {
            _libraries[device] = library;
        } else {
            const auto errorStr = (error && *error)
                ? (*error)->localizedDescription()->utf8String()
                : "unknown error";
            spdlog::error("Metal shader compilation failed (VS={}, FS={}): {}",
                vertexEntry(), fragmentEntry(), errorStr);
        }

        return library;
    }
}
