// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
#include "slangCompiler.h"

#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <sstream>
#include <vector>

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
        // A global session holds the core module; sessions made from it are per call. Slang
        // documents neither as safe to use from two threads at once, so the global sessions
        // are a POOL: a compile borrows a free one and gives it back, and another is created
        // only when every one is in use, by compiles running at once on several threads
        // (ProgramLibrary::requestSlangProgram). Used from one thread, the pool holds one.
        class GlobalSessionPool
        {
        public:
            /// A free global session, created if none is; null if Slang cannot create one.
            Slang::ComPtr<slang::IGlobalSession> acquire()
            {
                {
                    const std::lock_guard lock(_mutex);
                    if (!_free.empty()) {
                        Slang::ComPtr<slang::IGlobalSession> session = _free.back();
                        _free.pop_back();
                        return session;
                    }
                    if (_failed) {
                        return {};
                    }
                }
                // Created outside the lock: it loads the core module, which takes a while,
                // and another thread may give a session back meanwhile.
                Slang::ComPtr<slang::IGlobalSession> created;
                if (SLANG_FAILED(slang::createGlobalSession(created.writeRef())) || !created) {
                    const std::lock_guard lock(_mutex);
                    if (!_failed) {
                        spdlog::error("SlangCompiler: the Slang global session could not be created");
                    }
                    _failed = true;
                    return {};
                }
                return created;
            }

            void release(Slang::ComPtr<slang::IGlobalSession> session)
            {
                if (session) {
                    const std::lock_guard lock(_mutex);
                    _free.push_back(std::move(session));
                }
            }

        private:
            std::mutex _mutex;
            std::vector<Slang::ComPtr<slang::IGlobalSession>> _free;
            bool _failed = false;
        };

        GlobalSessionPool& globalSessions()
        {
            static GlobalSessionPool pool;
            return pool;
        }

        /// A global session borrowed for one compile, given back when it goes.
        class GlobalSessionLease
        {
        public:
            GlobalSessionLease() : _session(globalSessions().acquire()) {}
            ~GlobalSessionLease() { globalSessions().release(std::move(_session)); }
            GlobalSessionLease(const GlobalSessionLease&) = delete;
            GlobalSessionLease& operator=(const GlobalSessionLease&) = delete;
            slang::IGlobalSession* get() const { return _session.get(); }

        private:
            Slang::ComPtr<slang::IGlobalSession> _session;
        };

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
        const GlobalSessionLease lease;
        return lease.get() != nullptr;
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

        // Held to the end of the compile: the session made from it, and everything made from
        // that, belong to it.
        const GlobalSessionLease lease;
        auto* global = lease.get();
        if (!global) {
            result.diagnostics = "the Slang global session is unavailable";
            return finish();
        }

        // The engine's matrices are sixteen column-major floats (Matrix4::store). Read
        // ROW-major, Slang sees each stored column as a row, i.e. the transpose, so a shader
        // writes `mul(v, M)` for the engine's M * v. That spelling is what both targets emit
        // natively: MSL `M * v` and SPIR-V OpMatrixTimesVector, exactly what hand-written MSL
        // and GLSL produced. The other spelling (column-major, `mul(M, v)`) is the same maths,
        // but Slang emits it on Metal as a row vector times a rebuilt transposed matrix, which
        // rounds differently (TAA history amplified it to 34 counts at silhouettes).
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
        sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_ROW_MAJOR;
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

        // Each compute entry's workgroup size, which a dispatch sized by thread count needs
        // and which MSL does not carry.
        if (slang::ProgramLayout* layout = linked->getLayout()) {
            for (size_t i = 0; i < entryPoints.size(); ++i) {
                std::array<uint32_t, 3> size{};
                slang::EntryPointReflection* entry = layout->getEntryPointByIndex(static_cast<SlangUInt>(i));
                if (entry && entry->getStage() == SLANG_STAGE_COMPUTE) {
                    SlangUInt sizes[3] = {0, 0, 0};
                    entry->getComputeThreadGroupSize(3, sizes);
                    size = {static_cast<uint32_t>(sizes[0]), static_cast<uint32_t>(sizes[1]),
                        static_cast<uint32_t>(sizes[2])};
                }
                result.entryPointThreadGroupSizes.push_back(size);
            }
        }

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
            if (request.target == SlangTarget::Msl) {
                // The read-only buffers, from the linked program's layout.
                std::vector<std::string> readOnly;
                if (slang::ProgramLayout* layout = linked->getLayout()) {
                    for (unsigned i = 0; i < layout->getParameterCount(); ++i) {
                        slang::VariableLayoutReflection* parameter = layout->getParameterByIndex(i);
                        slang::TypeReflection* type = parameter ? parameter->getType() : nullptr;
                        if (!type || type->getKind() != slang::TypeReflection::Kind::Resource ||
                            type->getResourceAccess() != SLANG_RESOURCE_ACCESS_READ || !parameter->getName()) {
                            continue;
                        }
                        const auto shape = type->getResourceShape() & SLANG_RESOURCE_BASE_SHAPE_MASK;
                        if (shape == SLANG_STRUCTURED_BUFFER || shape == SLANG_BYTE_ADDRESS_BUFFER) {
                            readOnly.emplace_back(parameter->getName());
                        }
                    }
                }
                const std::string text = dualSourceOutputs(constReadOnlyBuffers(result.programText(), readOnly));
                result.programCode.assign(text.begin(), text.end());
            }
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

    namespace
    {
        // The character classes of the ECMAScript expressions these passes replaced.
        bool isWordChar(const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }
        bool isSpaceChar(const char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
        }
        bool isDigitChar(const char c)
        {
            return c >= '0' && c <= '9';
        }

        // At `pos`, one of `names` followed by `_`, digits and a word boundary (Slang's
        // `name_N`): the end of it, or npos.
        size_t matchBufferName(const std::string& text, const size_t pos, const std::vector<std::string>& names)
        {
            for (const auto& name : names) {
                if (name.empty() || text.compare(pos, name.size(), name) != 0) {
                    continue;
                }
                size_t at = pos + name.size();
                if (at >= text.size() || text[at] != '_') {
                    continue;
                }
                const size_t digits = ++at;
                while (at < text.size() && isDigitChar(text[at])) {
                    ++at;
                }
                if (at > digits && (at == text.size() || !isWordChar(text[at]))) {
                    return at;
                }
            }
            return std::string::npos;
        }

        size_t skipSpaces(const std::string& text, size_t at)
        {
            while (at < text.size() && isSpaceChar(text[at])) {
                ++at;
            }
            return at;
        }

        size_t skipWord(const std::string& text, size_t at)
        {
            while (at < text.size() && isWordChar(text[at])) {
                ++at;
            }
            return at;
        }

        // After a `device*` ending at `at`: a declaration of one of the read-only buffers
        // (`device* name_N`), or a local pointer Slang copies one into
        // (`device* _S1 = context->name_N`, `= (&x)->name_N`, `= name_N`).
        bool readsReadOnlyBuffer(const std::string& text, const size_t at, const std::vector<std::string>& names)
        {
            const size_t afterSpaces = skipSpaces(text, at);
            if (afterSpaces == at) {
                return false;   // `\s+`
            }
            if (matchBufferName(text, afterSpaces, names) != std::string::npos) {
                return true;
            }
            // `\w+\s*=\s*`, then an optional `(&?\w+)->` or `\w+->`, then the name.
            const size_t localEnd = skipWord(text, afterSpaces);
            if (localEnd == afterSpaces) {
                return false;
            }
            size_t cursor = skipSpaces(text, localEnd);
            if (cursor >= text.size() || text[cursor] != '=') {
                return false;
            }
            cursor = skipSpaces(text, cursor + 1);
            size_t prefixed = std::string::npos;
            if (cursor < text.size() && text[cursor] == '(') {
                size_t p = cursor + 1;
                if (p < text.size() && text[p] == '&') {
                    ++p;
                }
                const size_t wordEnd = skipWord(text, p);
                if (wordEnd > p && text.compare(wordEnd, 3, ")->") == 0) {
                    prefixed = wordEnd + 3;
                }
            } else {
                const size_t wordEnd = skipWord(text, cursor);
                if (wordEnd > cursor && text.compare(wordEnd, 2, "->") == 0) {
                    prefixed = wordEnd + 2;
                }
            }
            if (prefixed != std::string::npos && matchBufferName(text, prefixed, names) != std::string::npos) {
                return true;
            }
            return matchBufferName(text, cursor, names) != std::string::npos;
        }
    }

    std::string SlangCompiler::constReadOnlyBuffers(std::string msl, const std::vector<std::string>& readOnly)
    {
        if (readOnly.empty()) {
            return msl;
        }
        // Every `device*` (at a word boundary) that declares a read-only buffer, or a local
        // pointer copied from one, becomes `const device*`. Scanned by hand: std::regex over a
        // whole program (some 440 KB of MSL for the forward program) took seconds per compile.
        static constexpr std::string_view kDevice = "device*";
        std::string out;
        out.reserve(msl.size() + 256);
        size_t copied = 0;
        for (size_t at = msl.find(kDevice); at != std::string::npos; at = msl.find(kDevice, at + kDevice.size())) {
            if (at > 0 && isWordChar(msl[at - 1])) {
                continue;
            }
            if (readsReadOnlyBuffer(msl, at + kDevice.size(), readOnly)) {
                out.append(msl, copied, at - copied);
                out += "const ";
                copied = at;
            }
        }
        out.append(msl, copied, std::string::npos);
        return out;
    }

    std::string SlangCompiler::dualSourceOutputs(std::string msl)
    {
        // `dualSourceSecondary_N [[color(1)]]` (at a word boundary) takes `[[color(0), index(1)]]`.
        static constexpr std::string_view kName = "dualSourceSecondary_";
        static constexpr std::string_view kColor1 = "[[color(1)]]";
        std::string out;
        size_t copied = 0;
        for (size_t at = msl.find(kName); at != std::string::npos; at = msl.find(kName, at + kName.size())) {
            if (at > 0 && isWordChar(msl[at - 1])) {
                continue;
            }
            size_t cursor = at + kName.size();
            const size_t digits = cursor;
            while (cursor < msl.size() && isDigitChar(msl[cursor])) {
                ++cursor;
            }
            if (cursor == digits) {
                continue;
            }
            cursor = skipSpaces(msl, cursor);
            if (msl.compare(cursor, kColor1.size(), kColor1) == 0) {
                out.append(msl, copied, cursor - copied);
                out += "[[color(0), index(1)]]";
                copied = cursor + kColor1.size();
            }
        }
        if (copied == 0) {
            return msl;
        }
        out.append(msl, copied, std::string::npos);
        return out;
    }

}
