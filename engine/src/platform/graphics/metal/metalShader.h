// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2025
//
#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <Metal/Metal.hpp>
#include <Foundation/NSBundle.hpp>

#include "platform/graphics/shader.h"

namespace visutwin::canvas
{
    /**
     * Metal shader implementation.
     * Manages MTL::Library and MTL::Function objects for vertex and fragment shaders.
     *
     * The library starts compiling when the shader is CREATED, asynchronously, and
     * getLibrary() waits for it. Compiling MSL source is the whole cost of a shader the
     * system has not cached (about 0.4 s each), and compiling on first use makes a frame
     * that needs sixteen new shaders wait for them one after another; started at creation
     * they compile side by side, and the frame waits for the slowest, not the sum. So
     * whoever needs several new shaders should CREATE them all before using the first
     * (the forward pass resolves a layer's variants ahead of its draw loop for this).
     */
    class MetalShader : public Shader
    {
    public:
        MetalShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition, std::string sourceCode = "");
        ~MetalShader() override;

        MTL::Library* getLibrary(MTL::Device* device, const NS::Bundle* bundle, NS::Error** error);

    private:
        // The compile started at construction. Shared with its completion handler, which
        // Metal calls on a queue of its own and possibly after the shader is gone.
        struct Compile
        {
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
            bool abandoned = false;            // the shader was destroyed first
            MTL::Library* library = nullptr;   // retained; taken over by getLibrary
            std::string error;
        };

        MTL::CompileOptions* newCompileOptions() const;
        void reportCompileFailure(const std::string& reason) const;

        std::string _sourceCode;
        std::unordered_map<MTL::Device*, MTL::Library*> _libraries;
        std::shared_ptr<Compile> _compile;
        MTL::Device* _compileDevice = nullptr;
    };
}
