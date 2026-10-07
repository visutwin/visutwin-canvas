// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
#include "slangCompiler.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "spdlog/spdlog.h"

#ifdef VISUTWIN_HAS_SLANG
#include <slang.h>
#include <slang-com-ptr.h>
#include <slang-tag-version.h>
#endif

namespace visutwin::canvas
{
    std::vector<uint32_t> SlangCompileResult::spirv(const size_t entryPoint) const
    {
        std::vector<uint32_t> words;
        if (entryPoint >= entryPointCode.size()) {
            return words;
        }
        const auto& bytes = entryPointCode[entryPoint];
        words.resize(bytes.size() / sizeof(uint32_t));
        std::memcpy(words.data(), bytes.data(), words.size() * sizeof(uint32_t));
        return words;
    }

#ifdef VISUTWIN_HAS_SLANG
    namespace
    {
        // The global session holds the core module and is shared by every compile; it
        // is created once. Sessions made from it are per call: Slang documents them as
        // not safe to use concurrently.
        slang::IGlobalSession* globalSession()
        {
            static Slang::ComPtr<slang::IGlobalSession> session = [] {
                Slang::ComPtr<slang::IGlobalSession> created;
                if (SLANG_FAILED(slang::createGlobalSession(created.writeRef()))) {
                    spdlog::error("SlangCompiler: the Slang global session could not be created");
                }
                return created;
            }();
            return session.get();
        }

        std::string blobText(slang::IBlob* blob)
        {
            if (!blob || blob->getBufferSize() == 0) {
                return {};
            }
            return std::string(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
        }

        std::vector<uint8_t> blobBytes(slang::IBlob* blob)
        {
            if (!blob) {
                return {};
            }
            const auto* begin = static_cast<const uint8_t*>(blob->getBufferPointer());
            return std::vector<uint8_t>(begin, begin + blob->getBufferSize());
        }

        void appendDiagnostics(std::string& into, slang::IBlob* blob)
        {
            const std::string text = blobText(blob);
            if (!text.empty()) {
                into += text;
                if (text.back() != '\n') {
                    into += '\n';
                }
            }
        }

        // Serves the request's virtual files by file name and everything else from disk at
        // the path Slang resolved against the search paths. One per compile; Slang holds a
        // reference for the session's life, so the count is real.
        class MemoryFileSystem final : public ISlangFileSystem
        {
        public:
            explicit MemoryFileSystem(const std::vector<std::pair<std::string, std::string>>& files)
                : _files(files)
            {
            }

            SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& uuid, void** outObject) override
            {
                if (uuid == ISlangUnknown::getTypeGuid() || uuid == ISlangCastable::getTypeGuid() ||
                    uuid == ISlangFileSystem::getTypeGuid()) {
                    addRef();
                    *outObject = static_cast<ISlangFileSystem*>(this);
                    return SLANG_OK;
                }
                *outObject = nullptr;
                return SLANG_E_NO_INTERFACE;
            }
            SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++_refCount; }
            SLANG_NO_THROW uint32_t SLANG_MCALL release() override
            {
                const uint32_t count = --_refCount;
                if (count == 0) {
                    delete this;
                }
                return count;
            }
            SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID& uuid) override
            {
                if (uuid == ISlangUnknown::getTypeGuid() || uuid == ISlangCastable::getTypeGuid() ||
                    uuid == ISlangFileSystem::getTypeGuid()) {
                    return static_cast<ISlangFileSystem*>(this);
                }
                return nullptr;
            }
            SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char* path, ISlangBlob** outBlob) override
            {
                const std::string name = std::filesystem::path(path).filename().string();
                for (const auto& [fileName, text] : _files) {
                    if (fileName == name) {
                        *outBlob = slang_createBlob(text.data(), text.size());
                        return SLANG_OK;
                    }
                }
                std::ifstream in(path, std::ios::in | std::ios::binary);
                if (!in) {
                    return SLANG_E_NOT_FOUND;
                }
                std::ostringstream buffer;
                buffer << in.rdbuf();
                const std::string text = buffer.str();
                *outBlob = slang_createBlob(text.data(), text.size());
                return SLANG_OK;
            }

        private:
            std::vector<std::pair<std::string, std::string>> _files;
            uint32_t _refCount = 0;
        };

        SlangCompileTarget targetFormat(const SlangTarget target)
        {
            switch (target) {
            case SlangTarget::Msl: return SLANG_METAL;
            case SlangTarget::MetalLib: return SLANG_METAL_LIB;
            case SlangTarget::Spirv: return SLANG_SPIRV;
            case SlangTarget::Wgsl: return SLANG_WGSL;
            }
            return SLANG_TARGET_UNKNOWN;
        }
    }

    bool SlangCompiler::available()
    {
        return globalSession() != nullptr;
    }

    const char* SlangCompiler::version()
    {
        return SLANG_TAG_VERSION;
    }

    SlangCompileResult SlangCompiler::compile(const SlangCompileRequest& request)
    {
        SlangCompileResult result;
        const auto started = std::chrono::steady_clock::now();
        auto finish = [&] {
            result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            return result;
        };

        auto* global = globalSession();
        if (!global) {
            result.diagnostics = "the Slang global session is unavailable";
            return finish();
        }

        // The engine's matrices are column-major on both backends (a Matrix4 is sixteen
        // column-major floats), so `mul(M, v)` must read M as columns. Slang's default
        // is HLSL's row-major; left at that, every transform would be transposed.
        slang::TargetDesc targetDesc{};
        targetDesc.format = targetFormat(request.target);

        std::vector<slang::CompilerOptionEntry> options;
        if (request.debugInfo) {
            slang::CompilerOptionEntry debug{};
            debug.name = slang::CompilerOptionName::DebugInformation;
            debug.value.kind = slang::CompilerOptionValueKind::Int;
            debug.value.intValue0 = SLANG_DEBUG_INFO_LEVEL_STANDARD;
            options.push_back(debug);
        }

        std::vector<slang::PreprocessorMacroDesc> macros;
        macros.reserve(request.defines.size() + 1);
        for (const auto& [name, value] : request.defines) {
            macros.push_back({name.c_str(), value.c_str()});
        }
        // The target the declarations are laid out for: bindings.slang declares some inputs
        // differently per backend (the build's bundle passes the same define to slangc).
        if (request.target == SlangTarget::Msl || request.target == SlangTarget::MetalLib) {
            macros.push_back({"VT_TARGET_METAL", "1"});
        }
        std::vector<const char*> searchPaths;
        searchPaths.reserve(request.searchPaths.size());
        for (const auto& path : request.searchPaths) {
            searchPaths.push_back(path.c_str());
        }

        Slang::ComPtr<ISlangFileSystem> fileSystem;
        if (!request.virtualFiles.empty()) {
            auto* memory = new MemoryFileSystem(request.virtualFiles);
            memory->addRef();
            fileSystem.attach(memory);
        }

        slang::SessionDesc sessionDesc{};
        sessionDesc.fileSystem = fileSystem.get();
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
        sessionDesc.preprocessorMacros = macros.empty() ? nullptr : macros.data();
        sessionDesc.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
        sessionDesc.searchPaths = searchPaths.empty() ? nullptr : searchPaths.data();
        sessionDesc.searchPathCount = static_cast<SlangInt>(searchPaths.size());
        sessionDesc.compilerOptionEntries = options.empty() ? nullptr : options.data();
        sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());

        Slang::ComPtr<slang::ISession> session;
        if (SLANG_FAILED(global->createSession(sessionDesc, session.writeRef())) || !session) {
            result.diagnostics = "the Slang session could not be created";
            return finish();
        }

        Slang::ComPtr<slang::IBlob> diagnostics;
        const std::string path = request.moduleName + ".slang";
        slang::IModule* module = session->loadModuleFromSourceString(
            request.moduleName.c_str(), path.c_str(), request.source.c_str(), diagnostics.writeRef());
        appendDiagnostics(result.diagnostics, diagnostics);
        if (!module) {
            return finish();
        }

        std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints;
        std::vector<slang::IComponentType*> components;
        components.push_back(module);
        for (const auto& name : request.entryPoints) {
            Slang::ComPtr<slang::IEntryPoint> entryPoint;
            if (SLANG_FAILED(module->findEntryPointByName(name.c_str(), entryPoint.writeRef())) || !entryPoint) {
                result.diagnostics += "entry point '" + name + "' is not declared in module '" + request.moduleName + "'\n";
                return finish();
            }
            components.push_back(entryPoint.get());
            entryPoints.push_back(std::move(entryPoint));
        }

        Slang::ComPtr<slang::IComponentType> composite;
        diagnostics = nullptr;
        if (SLANG_FAILED(session->createCompositeComponentType(components.data(),
                static_cast<SlangInt>(components.size()), composite.writeRef(), diagnostics.writeRef()))) {
            appendDiagnostics(result.diagnostics, diagnostics);
            return finish();
        }
        appendDiagnostics(result.diagnostics, diagnostics);

        Slang::ComPtr<slang::IComponentType> linked;
        diagnostics = nullptr;
        if (SLANG_FAILED(composite->link(linked.writeRef(), diagnostics.writeRef()))) {
            appendDiagnostics(result.diagnostics, diagnostics);
            return finish();
        }
        appendDiagnostics(result.diagnostics, diagnostics);

        if (request.target == SlangTarget::Spirv) {
            for (size_t i = 0; i < entryPoints.size(); ++i) {
                Slang::ComPtr<slang::IBlob> code;
                diagnostics = nullptr;
                const SlangResult rc = linked->getEntryPointCode(static_cast<SlangInt>(i), 0, code.writeRef(),
                    diagnostics.writeRef());
                appendDiagnostics(result.diagnostics, diagnostics);
                if (SLANG_FAILED(rc) || !code) {
                    return finish();
                }
                result.entryPointCode.push_back(blobBytes(code));
            }
        } else {
            Slang::ComPtr<slang::IBlob> code;
            diagnostics = nullptr;
            const SlangResult rc = linked->getTargetCode(0, code.writeRef(), diagnostics.writeRef());
            appendDiagnostics(result.diagnostics, diagnostics);
            if (SLANG_FAILED(rc) || !code) {
                return finish();
            }
            result.programCode = blobBytes(code);
        }

        result.ok = true;
        return finish();
    }
#else
    bool SlangCompiler::available()
    {
        return false;
    }

    const char* SlangCompiler::version()
    {
        return "unavailable";
    }

    SlangCompileResult SlangCompiler::compile(const SlangCompileRequest& request)
    {
        SlangCompileResult result;
        result.diagnostics = "module '" + request.moduleName +
            "': this build has no Slang compiler (VISUTWIN_SHADER_SLANG is off or the package is missing)";
        return result;
    }
#endif
}
