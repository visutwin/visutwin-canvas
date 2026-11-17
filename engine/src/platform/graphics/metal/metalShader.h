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
     *
     * Shaders with the SAME source share one compile: every variant of a run-time Slang
     * program is the program's whole MSL with its features bound later as function constants
     * (newFunction), so the forward program's 49 variants in `shader-precompile` were 49
     * compiles of one 440 KB text. A shader finds a compile of its source still running or
     * still held by another shader (on its MTL::Device, the text compared exactly) and joins
     * it; each shader retains the library for itself once it takes it.
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

        bool waitUntilCompiled() override;

    protected:
        void swapBackendCode(Shader& other) override;

    private:
    public:
        // The compile started at construction. Shared with its completion handler, which
        // Metal calls on a queue of its own and possibly after the shader is gone, and with
        // every shader of the same source (sharedCompile in metalShader.cpp).
        struct Compile
        {
            ~Compile();
            std::mutex mutex;
            std::condition_variable done;
            bool finished = false;
            MTL::Library* library = nullptr;   // retained, released with the compile
            std::string error;
            MTL::Device* device = nullptr;
            std::string source;                // what was compiled, compared on a lookup
        };

    private:

        MTL::CompileOptions* newCompileOptions() const;
        void reportCompileFailure(const std::string& reason) const;

        std::string _sourceCode;
        std::unordered_map<MTL::Device*, MTL::Library*> _libraries;
        // getLibrary runs on the render thread and on the compile threads of a precompile
        // (MetalRenderPipeline::request), so the library map and the pending compile are
        // taken under this.
        std::mutex _libraryMutex;
        std::shared_ptr<Compile> _compile;
        MTL::Device* _compileDevice = nullptr;
        bool _specializeFeatures = false;
    };
}
