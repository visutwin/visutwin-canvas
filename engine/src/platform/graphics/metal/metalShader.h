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
#include <vector>
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
        /// From a compiled Metal library (a metallib's bytes), loaded at once.
        MetalShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition,
            const std::vector<uint8_t>& library);
        ~MetalShader() override;

        MTL::Library* getLibrary(MTL::Device* device, const NS::Bundle* bundle, NS::Error** error);

        /// The entry point `name` from `library`. With specializeFeatures() the
        /// definition's feature words are supplied as function constants 0..N-1, the
        /// twin of the Vulkan backend's specialization constants, so a library compiled
        /// once serves every variant. Null (and `error` set) when the function is missing.
        MTL::Function* newFunction(MTL::Library* library, const std::string& name, NS::Error** error) const;

        /// Whether newFunction binds the feature words as function constants. Off for a
        /// shader whose source carries its variant as preprocessor defines.
        void setSpecializeFeatures(const bool value) { _specializeFeatures = value; }
        [[nodiscard]] bool specializesFeatures() const { return _specializeFeatures; }

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
        bool _specializeFeatures = false;
    };
}
