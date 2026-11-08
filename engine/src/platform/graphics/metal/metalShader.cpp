// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.10.2025
//
#include "metalShader.h"

#include <cstdlib>
#include <fstream>
#include <utility>

#include <dispatch/dispatch.h>

#include "metalGraphicsDevice.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    MetalShader::MetalShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition, std::string sourceCode)
        : Shader(graphicsDevice, definition), _sourceCode(std::move(sourceCode))
    {
        // Start compiling now (see the class comment). A shader with no source, or made
        // for something that is not a Metal device, compiles on demand in getLibrary.
        auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(graphicsDevice);
        MTL::Device* device = metalDevice ? metalDevice->raw() : nullptr;
        if (!device || _sourceCode.empty()) {
            return;
        }
        // VISUTWIN_METAL_DUMP_MSL=<dir> writes every source this device compiles to
        // <dir>/<shader name>.metal, for a diff against the bundle's work files.
        if (const char* dumpDir = std::getenv("VISUTWIN_METAL_DUMP_MSL"); dumpDir && dumpDir[0]) {
            std::string fileName = definition.name;
            for (char& c : fileName) {
                if (c == '/' || c == ':' || c == ' ') {
                    c = '_';
                }
            }
            if (std::ofstream out(std::string(dumpDir) + "/" + fileName + ".metal"); out) {
                out << _sourceCode;
            }
        }

        _compileDevice = device;
        _compile = std::make_shared<Compile>();
        auto* compileOptions = newCompileOptions();
        device->newLibrary(NS::String::string(_sourceCode.c_str(), NS::UTF8StringEncoding), compileOptions,
            [state = _compile](MTL::Library* library, NS::Error* error) {
                const std::lock_guard lock(state->mutex);
                if (library && !state->abandoned) {
                    library->retain();
                    state->library = library;
                }
                if (!library) {
                    state->error = (error && error->localizedDescription())
                        ? error->localizedDescription()->utf8String() : "unknown error";
                }
                state->finished = true;
                state->done.notify_all();
            });
        compileOptions->release();
    }

    MetalShader::MetalShader(GraphicsDevice* graphicsDevice, const ShaderDefinition& definition,
        const std::vector<uint8_t>& library)
        : Shader(graphicsDevice, definition)
    {
        auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(graphicsDevice);
        MTL::Device* device = metalDevice ? metalDevice->raw() : nullptr;
        if (!device || library.empty()) {
            return;
        }
        // dispatch_data_create copies the bytes (DISPATCH_DATA_DESTRUCTOR_DEFAULT), so
        // the caller's vector may go.
        dispatch_data_t data = dispatch_data_create(library.data(), library.size(), nullptr,
            DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NS::Error* error = nullptr;
        MTL::Library* loaded = device->newLibrary(data, &error);
        dispatch_release(data);
        if (loaded) {
            _libraries[device] = loaded;
        } else {
            reportCompileFailure(std::string("metallib rejected: ") +
                ((error && error->localizedDescription()) ? error->localizedDescription()->utf8String()
                                                           : "unknown error"));
        }
    }

    MetalShader::~MetalShader()
    {
        if (_compile) {
            const std::lock_guard lock(_compile->mutex);
            // Still compiling: the handler drops the library when it arrives.
            _compile->abandoned = true;
            if (_compile->library) {
                _compile->library->release();
                _compile->library = nullptr;
            }
        }
        for (const auto& [_, library] : _libraries) {
            if (library) {
                library->release();
            }
        }
    }

    bool MetalShader::waitUntilCompiled()
    {
        auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(graphicsDevice());
        if (!metalDevice || !metalDevice->raw()) {
            return true;
        }
        NS::Error* error = nullptr;
        return getLibrary(metalDevice->raw(), nullptr, &error) != nullptr;
    }

    void MetalShader::swapBackendCode(Shader& other)
    {
        auto& fresh = static_cast<MetalShader&>(other);
        std::swap(_sourceCode, fresh._sourceCode);
        std::swap(_libraries, fresh._libraries);
        std::swap(_compile, fresh._compile);
        std::swap(_compileDevice, fresh._compileDevice);
        std::swap(_specializeFeatures, fresh._specializeFeatures);
    }

    MTL::Function* MetalShader::newFunction(MTL::Library* library, const std::string& name, NS::Error** error) const
    {
        if (!library) {
            return nullptr;
        }
        auto* functionName = NS::String::string(name.c_str(), NS::UTF8StringEncoding);
        if (!_specializeFeatures) {
            return library->newFunction(functionName);
        }
        // One constant per feature word, index i = word i, as the Vulkan pipeline's
        // VkSpecializationMapEntry table and the vtFeatureMask<N> declarations.
        auto* constants = MTL::FunctionConstantValues::alloc()->init();
        const auto& words = features().words();
        constants->setConstantValues(words.data(), MTL::DataTypeUInt,
            NS::Range::Make(0, static_cast<NS::UInteger>(words.size())));
        MTL::Function* function = library->newFunction(functionName, constants, error);
        constants->release();
        return function;
    }

    MTL::CompileOptions* MetalShader::newCompileOptions() const
    {
        auto* compileOptions = MTL::CompileOptions::alloc()->init();
        // The RELAXED math mode, the one the bundle's metallibs are built with
        // (tools/generate_slang_bundle.py), so a runtime compile of a program or an override
        // is bit-identical to its bundled twin. The deprecated fastMathEnabled is ignored on
        // macOS 27, and the fast mode's lossy optimisations drew a blended glass up to 30%
        // darker than the relaxed compile of the same MSL, which MoltenVK agreed with.
        compileOptions->setMathMode(MTL::MathModeRelaxed);
        return compileOptions;
    }

    void MetalShader::reportCompileFailure(const std::string& reason) const
    {
        spdlog::error("Metal shader compilation failed (VS={}, FS={}): {}",
            vertexEntry(), fragmentEntry(), reason);
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

        // The compile started at construction: wait for it and take the library over.
        if (_compile && device == _compileDevice) {
            MTL::Library* library = nullptr;
            std::string failure;
            {
                std::unique_lock lock(_compile->mutex);
                _compile->done.wait(lock, [this] { return _compile->finished; });
                library = _compile->library;
                _compile->library = nullptr;
                failure = _compile->error;
            }
            _compile.reset();
            if (library) {
                _libraries[device] = library;
            } else {
                reportCompileFailure(failure.empty() ? "unknown error" : failure);
            }
            return library;
        }

        if (_sourceCode.empty()) {
            spdlog::error("MetalShader source is empty. Source-less metallib fallback was removed.");
            return nullptr;
        }

        auto* compileOptions = newCompileOptions();
        MTL::Library* library = device->newLibrary(NS::String::string(_sourceCode.c_str(), NS::UTF8StringEncoding),
            compileOptions, error);
        compileOptions->release();

        if (library) {
            _libraries[device] = library;
        } else {
            reportCompileFailure((error && *error)
                ? (*error)->localizedDescription()->utf8String()
                : "unknown error");
        }

        return library;
    }
}
