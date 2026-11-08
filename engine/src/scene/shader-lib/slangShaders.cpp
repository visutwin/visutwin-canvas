// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
#include "slangShaders.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <vector>

#include "spdlog/spdlog.h"

#include "core/hash.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/shaderDiskCache.h"
#include "platform/graphics/slangCompiler.h"
#include "scene/shader-lib/shaderChunks.h"

#ifdef VISUTWIN_HAS_SLANG
#include "slang/slang_shader_bundle.h"
#endif

namespace visutwin::canvas
{
    namespace
    {
        std::string readTextFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in) {
                return {};
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        /// The source tree holding the programs and modules, or an empty path.
        std::filesystem::path slangSourceDir()
        {
            for (const auto& root : shaderSourceRoots()) {
                const auto dir = root / "engine" / "shaders" / "slang";
                if (std::filesystem::is_directory(dir / "programs")) {
                    return dir;
                }
            }
            return {};
        }

        /// A program read with every file it #includes: its entry points in source order and
        /// the text of every file read (for the cache key). A file named in `overrides` is
        /// read from there instead of the tree.
        struct SourceScan
        {
            std::string programText;
            std::vector<std::pair<std::string, std::string>> entries;   // stage, name
            std::string keyMaterial;
        };

        void scanSource(const std::filesystem::path& path, const std::filesystem::path& dir,
            const SlangFileOverrides& overrides, std::vector<std::filesystem::path>& seen, SourceScan& scan,
            const bool isProgram)
        {
            const auto canonical = std::filesystem::weakly_canonical(path);
            if (std::find(seen.begin(), seen.end(), canonical) != seen.end()) {
                return;
            }
            seen.push_back(canonical);
            std::string text;
            const std::string fileName = path.filename().string();
            const auto overridden = std::find_if(overrides.begin(), overrides.end(),
                [&fileName](const auto& entry) { return entry.first == fileName; });
            text = overridden != overrides.end() ? overridden->second : readTextFile(path);
            if (isProgram) {
                scan.programText = text;
            }
            scan.keyMaterial += "file " + fileName + "\n" + text + "\n";

            static const std::regex include(R"re(^[ \t]*#[ \t]*include[ \t]+"([^"]+)")re", std::regex::multiline);
            for (auto it = std::sregex_iterator(text.begin(), text.end(), include); it != std::sregex_iterator(); ++it) {
                const std::string name = (*it)[1];
                for (const auto& base : {path.parent_path(), dir, dir / "modules"}) {
                    if (std::filesystem::exists(base / name)) {
                        scanSource(base / name, dir, overrides, seen, scan, false);
                        break;
                    }
                }
            }
            // A delimited raw string: the pattern itself contains `)"`.
            static const std::regex entry(R"re(\[shader\("(vertex|fragment|compute)"\)\]\s*(?:\[[^\]]*\]\s*)*[\w<>:]+\s+(\w+)\s*\()re");
            for (auto it = std::sregex_iterator(text.begin(), text.end(), entry); it != std::sregex_iterator(); ++it) {
                scan.entries.emplace_back((*it)[1], (*it)[2]);
            }
        }

        /// The defines a program's `// @variant <name>: ...` line gives `variant`; nothing
        /// (and false) when the program declares variants but not this one. A program with
        /// no @variant lines has only "".
        bool variantDefines(const std::string& source, const std::string_view variant,
            std::vector<std::pair<std::string, std::string>>& defines)
        {
            static const std::regex pattern(R"re(//\s*@variant\s+([\w.-]+)\s*:([^\n]*))re");
            bool any = false;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), pattern); it != std::sregex_iterator(); ++it) {
                any = true;
                if ((*it)[1].str() != variant) {
                    continue;
                }
                std::istringstream words((*it)[2].str());
                std::string word;
                while (words >> word) {
                    const auto eq = word.find('=');
                    defines.emplace_back(word.substr(0, eq), eq == std::string::npos ? "1" : word.substr(eq + 1));
                }
                return true;
            }
            return !any && variant.empty();
        }

        bool envFlag(const char* name)
        {
            const char* value = std::getenv(name);
            return value && *value && std::strcmp(value, "0") != 0;
        }

        // The watcher recompiles from the tree, so watching implies compiling from it.
        bool wantsRuntimeCompile()
        {
            return envFlag("VISUTWIN_SLANG_RUNTIME") || envFlag("VISUTWIN_SHADER_WATCH");
        }

        std::string hex(const uint64_t value)
        {
            char text[17];
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
            return text;
        }
    }

    bool findBundledSlangProgram(const std::string_view program, SlangBundledProgram& out,
        const std::string_view variant)
    {
#ifdef VISUTWIN_HAS_SLANG
        const auto* entry = slang_generated::findProgram(program, variant);
        if (!entry) {
            return false;
        }
        out.name = entry->name;
        out.vertexEntry = entry->vertexEntry;
        out.fragmentEntry = entry->fragmentEntry;
        out.computeEntry = entry->computeEntry;
        out.metalSource = entry->metalSource;
        out.metalLibrary = entry->metalLibrary;
        out.metalLibraryBytes = entry->metalLibraryBytes;
        out.vertexSpirv = entry->vertexSpirv;
        out.vertexSpirvWords = entry->vertexSpirvWords;
        out.fragmentSpirv = entry->fragmentSpirv;
        out.fragmentSpirvWords = entry->fragmentSpirvWords;
        out.computeSpirv = entry->computeSpirv;
        out.computeSpirvWords = entry->computeSpirvWords;
        out.entries.clear();
        for (size_t i = 0; i < entry->entryCount; ++i) {
            const auto& e = entry->entries[i];
            SlangBundledProgram::Entry copy{e.stage, e.name, e.spirv, e.spirvWords, {}, e.pushConstantBytes,
                {e.threadGroupSize[0], e.threadGroupSize[1], e.threadGroupSize[2]}};
            for (size_t b = 0; b < e.bindingCount; ++b) {
                const auto& r = e.bindings[b];
                copy.bindings.push_back({r.name, r.set, r.binding,
                    static_cast<SlangDescriptorKind>(static_cast<uint8_t>(r.kind)), r.blockBytes});
            }
            out.entries.push_back(std::move(copy));
        }
        return true;
#else
        (void)program;
        (void)out;
        (void)variant;
        return false;
#endif
    }

    namespace
    {
#ifdef VISUTWIN_HAS_SLANG
        /// Every source a compile of `program` / `variant` reads: the program and every file
        /// it includes (`scan`), every module (an import closure is not reported, and the tree
        /// is small) and the bindings. The persistent cache keys on it; the watcher compares it.
        std::string sourceKeyMaterial(const std::filesystem::path& dir, const std::string_view program,
            const std::string_view variant, const SourceScan& scan)
        {
            std::string key = "program " + std::string(program) + " variant " + std::string(variant) + "\n" +
                scan.keyMaterial;
            std::vector<std::filesystem::path> modules;
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(dir / "modules", error)) {
                if (entry.is_regular_file() && entry.path().extension() == ".slang") {
                    modules.push_back(entry.path());
                }
            }
            // Sorted, so the key does not depend on the directory's order.
            std::sort(modules.begin(), modules.end());
            for (const auto& path : modules) {
                key += "module " + path.filename().string() + "\n" + readTextFile(path) + "\n";
            }
            key += slang_generated::kBindingsSlang;
            return key;
        }
#endif

        bool compileProgramEntries(GraphicsDevice* device, const std::string_view program, const std::string_view variant,
            const SlangFileOverrides& overrides, SlangProgramBuild& out)
        {
#ifndef VISUTWIN_HAS_SLANG
            (void)device;
            (void)variant;
            (void)overrides;
            (void)out;
            spdlog::error("Slang program '{}': this build has no Slang compiler", program);
            return false;
#else
            if (!device) {
                return false;
            }
            const auto dir = slangSourceDir();
            if (dir.empty()) {
                spdlog::error("Slang program '{}': no engine/shaders/slang source tree found (VISUTWIN_CANVAS_SHADERS?)",
                    program);
                return false;
            }
            const auto path = dir / "programs" / (std::string(program) + ".slang");
            SourceScan scan;
            std::vector<std::filesystem::path> seen;
            scanSource(path, dir, overrides, seen, scan, true);
            if (scan.programText.empty()) {
                spdlog::error("Slang program '{}': {} is missing or empty", program, path.string());
                return false;
            }
            if (scan.entries.empty()) {
                spdlog::error("Slang program '{}': no [shader(...)] entry points in {} or its includes", program,
                    path.string());
                return false;
            }

            const bool metal = device->shaderLanguage() == ShaderLanguage::Msl;
            SlangCompileRequest request;
            if (!variantDefines(scan.programText, variant, request.defines)) {
                spdlog::error("Slang program '{}': {} declares no variant '{}'", program, path.string(), variant);
                return false;
            }
            request.moduleName = std::string(program);
            request.source = scan.programText;
            for (const auto& [stage, name] : scan.entries) {
                request.entryPoints.push_back(name);
            }
            request.target = metal ? SlangTarget::Msl : SlangTarget::Spirv;
            request.searchPaths = {dir.string(), (dir / "modules").string()};
            // The generated bindings.slang is not in the source tree: the bundle carries its
            // text. A file override is served the same way, by name.
            request.virtualFiles = {{"bindings.slang", slang_generated::kBindingsSlang}};
            for (const auto& entry : overrides) {
                request.virtualFiles.push_back(entry);
            }

            // Everything the output is a function of, for the device's persistent cache: the
            // Slang version, the target and the sources (sourceKeyMaterial).
            const std::string cacheKey = std::string("slang ") + SlangCompiler::version() +
                (metal ? " metal\n" : " spirv\n") + sourceKeyMaterial(dir, program, variant, scan);
            const std::string cacheName = std::string(program) + (variant.empty() ? "" : "@" + std::string(variant)) +
                "-" + hex(fnv1a64(cacheKey));

            out = {};
            for (const auto& [stage, name] : scan.entries) {
                out.entries.push_back({stage, name, {}});
            }

            const ShaderDiskCache* cache = device->shaderDiskCache();
            if (cache && !metal) {
                bool all = true;
                for (auto& entry : out.entries) {
                    const auto stored = cache->load("slang-spv", cacheName + "." + entry.name);
                    if (!stored || stored->empty() || stored->size() % sizeof(uint32_t) != 0) {
                        all = false;
                        break;
                    }
                    entry.spirv.resize(stored->size() / sizeof(uint32_t));
                    std::memcpy(entry.spirv.data(), stored->data(), stored->size());
                }
                if (all) {
                    return true;
                }
            }

            const auto result = SlangCompiler::compile(request);
            if (!result.ok) {
                spdlog::error("Slang program '{}' failed to compile for {}:\n{}", program, metal ? "Metal" : "Vulkan",
                    result.diagnostics);
                return false;
            }
            // A program handed over as a string (getOrCreateSlangShaderFromSource) has no file.
            const std::string programFile = std::string(program) + ".slang";
            const bool fromString = std::any_of(overrides.begin(), overrides.end(),
                [&programFile](const auto& entry) { return entry.first == programFile; });
            spdlog::info("Slang program '{}'{}{} compiled from {} for {} in {:.1f} ms", program,
                variant.empty() ? "" : " variant ", variant, fromString ? std::string("its source") : path.string(),
                metal ? "Metal" : "Vulkan", result.seconds * 1000.0);
            for (size_t i = 0; i < out.entries.size() && i < result.entryPointThreadGroupSizes.size(); ++i) {
                out.entries[i].threadGroupSize = result.entryPointThreadGroupSizes[i];
            }
            if (metal) {
                out.metalSource = result.programText();
                return true;
            }
            for (size_t i = 0; i < out.entries.size(); ++i) {
                out.entries[i].spirv = result.spirv(i);
                if (cache) {
                    const auto& words = out.entries[i].spirv;
                    cache->store("slang-spv", cacheName + "." + out.entries[i].name,
                        {reinterpret_cast<const uint8_t*>(words.data()), words.size() * sizeof(uint32_t)});
                }
            }
            return true;
#endif
        }

        /// The code of `build`'s first entry per stage, and its entry names in `definition`
        /// (where not set already).
        void makeCodeFromBuild(const SlangProgramBuild& build, ShaderDefinition& definition, ShaderCode& code)
        {
            code.specializeFeatures = true;
            code.metalSource = build.metalSource;
            for (const auto& entry : build.entries) {
                std::string& target = entry.stage == "vertex" ? definition.vshader
                    : entry.stage == "fragment" ? definition.fshader : definition.cshader;
                if (!target.empty()) {
                    continue;
                }
                target = entry.name;
                auto& spirv = entry.stage == "vertex" ? code.vertexSpirv
                    : entry.stage == "fragment" ? code.fragmentSpirv : code.computeSpirv;
                spirv = entry.spirv;
                if (entry.stage == "compute") {
                    code.computeThreadGroupSize = entry.threadGroupSize;
                }
            }
            code.depthOnlyFragment = spirvFragmentWritesNoOutput(code.fragmentSpirv);
        }

        /// The first entry of `stage`, or nothing.
        const SlangProgramBuild::Entry* firstEntry(const SlangProgramBuild& build, const std::string_view stage)
        {
            for (const auto& entry : build.entries) {
                if (entry.stage == stage) {
                    return &entry;
                }
            }
            return nullptr;
        }
    }

    bool compileSlangProgramFromSource(GraphicsDevice* device, const std::string_view program, ShaderCode& code,
        const std::string_view variant)
    {
        SlangProgramBuild build;
        if (!compileProgramEntries(device, program, variant, {}, build)) {
            return false;
        }
        code.specializeFeatures = true;
        code.metalSource = build.metalSource;
        if (const auto* vertex = firstEntry(build, "vertex")) {
            code.vertexSpirv = vertex->spirv;
        }
        if (const auto* fragment = firstEntry(build, "fragment")) {
            code.fragmentSpirv = fragment->spirv;
        }
        if (const auto* compute = firstEntry(build, "compute")) {
            code.computeSpirv = compute->spirv;
            code.computeThreadGroupSize = compute->threadGroupSize;
        }
        return true;
    }

    bool loadSlangProgram(GraphicsDevice* device, const std::string_view program, const std::string_view variant,
        const SlangFileOverrides& overrides, SlangProgramBuild& out)
    {
        if (!device) {
            return false;
        }
        SlangBundledProgram bundled;
        if (overrides.empty() && !wantsRuntimeCompile() && findBundledSlangProgram(program, bundled, variant)) {
            out = {};
            const bool metal = device->shaderLanguage() == ShaderLanguage::Msl;
            bool haveCode = true;
            if (metal) {
                if (bundled.metalLibrary && bundled.metalLibraryBytes > 0) {
                    out.metalLibrary.assign(bundled.metalLibrary, bundled.metalLibrary + bundled.metalLibraryBytes);
                }
                out.metalSource = std::string(bundled.metalSource);
                haveCode = !out.metalLibrary.empty() || !out.metalSource.empty();
            }
            for (const auto& entry : bundled.entries) {
                SlangProgramBuild::Entry e{std::string(entry.stage), std::string(entry.name), {},
                    entry.threadGroupSize};
                if (!metal) {
                    if (!entry.spirv) {
                        haveCode = false;
                    } else {
                        e.spirv.assign(entry.spirv, entry.spirv + entry.spirvWords);
                    }
                }
                out.entries.push_back(std::move(e));
            }
            if (haveCode && !out.entries.empty()) {
                return true;
            }
            spdlog::warn("Slang program '{}': the bundle holds no code for {}; compiling from source", program,
                metal ? "Metal" : "Vulkan");
        }
        return compileProgramEntries(device, program, variant, overrides, out);
    }

    bool spirvFragmentWritesNoOutput(const std::vector<uint32_t>& spirv)
    {
        // The header is five words; each instruction's first word holds its word count
        // (high half) and opcode (low half). OpVariable is 59, its storage class the
        // fourth word, and Output is storage class 3.
        constexpr uint32_t kOpVariable = 59;
        constexpr uint32_t kStorageClassOutput = 3;
        if (spirv.size() <= 5 || spirv[0] != 0x07230203u) {
            return false;
        }
        for (size_t i = 5; i < spirv.size();) {
            const uint32_t wordCount = spirv[i] >> 16;
            const uint32_t opcode = spirv[i] & 0xFFFFu;
            if (wordCount == 0 || i + wordCount > spirv.size()) {
                return false;
            }
            if (opcode == kOpVariable && wordCount >= 4 && spirv[i + 3] == kStorageClassOutput) {
                return false;
            }
            i += wordCount;
        }
        return true;
    }

    namespace
    {
        void recordLiveSlangShader(GraphicsDevice* device, const std::shared_ptr<Shader>& shader, std::string program,
            std::string variant, SlangFileOverrides files, uint64_t fingerprint);

        /// The fingerprint to record for a shader about to be compiled, taken BEFORE the
        /// compile: a change landing while it compiles then differs from it and is reloaded on
        /// the next poll. 0 while the watcher is off.
        uint64_t watchFingerprint(const std::string_view program, const std::string_view variant,
            const SlangFileOverrides& files)
        {
            return slangShaderWatchEnabled() ? slangSourceFingerprint(program, variant, files) : 0;
        }
    }

    std::shared_ptr<Shader> getOrCreateSlangShader(GraphicsDevice* device, const std::string& program,
        const std::string& variant, const ShaderFeatureSet& features)
    {
        if (!device) {
            return nullptr;
        }
        std::string cacheKey = "slang:" + program + (variant.empty() ? "" : "@" + variant);
        bool anyFeature = false;
        for (const uint32_t word : features.words()) {
            anyFeature = anyFeature || word != 0;
        }
        if (anyFeature) {
            cacheKey += ":" + hex(features.hash());
        }
        if (auto cached = device->getCachedShader(cacheKey)) {
            return cached;
        }

        const uint64_t fingerprint = watchFingerprint(program, variant, {});
        ShaderDefinition definition;
        definition.name = cacheKey;
        definition.features = features;
        ShaderCode code;
        code.specializeFeatures = true;

        SlangBundledProgram bundled;
        const bool inBundle = findBundledSlangProgram(program, bundled, variant);
        const bool metal = device->shaderLanguage() == ShaderLanguage::Msl;
        bool haveCode = false;
        if (inBundle && !wantsRuntimeCompile()) {
            definition.vshader = std::string(bundled.vertexEntry);
            definition.fshader = std::string(bundled.fragmentEntry);
            definition.cshader = std::string(bundled.computeEntry);
            for (const auto& entry : bundled.entries) {
                if (entry.stage == "compute" && entry.name == bundled.computeEntry) {
                    code.computeThreadGroupSize = entry.threadGroupSize;
                }
            }
            if (metal) {
                // The precompiled library, so the first use costs a load, not an MSL
                // compile; MetalShader takes the source only when no library is given.
                // VISUTWIN_METAL_BUNDLE_SOURCE=1 leaves the library aside, so Metal compiles the
                // bundle's own MSL at run time: tells a difference in the MSL from one in the
                // offline compile (xcrun metal) when the bundle and the runtime path disagree.
                static const bool bundleSource = [] {
                    const char* value = std::getenv("VISUTWIN_METAL_BUNDLE_SOURCE");
                    return value && value[0] == '1';
                }();
                if (bundled.metalLibrary && bundled.metalLibraryBytes > 0 && !bundleSource) {
                    code.metalLibrary.assign(bundled.metalLibrary, bundled.metalLibrary + bundled.metalLibraryBytes);
                }
                code.metalSource = std::string(bundled.metalSource);
                haveCode = !code.metalLibrary.empty() || !code.metalSource.empty();
            } else {
                code.vertexSpirv.assign(bundled.vertexSpirv, bundled.vertexSpirv + bundled.vertexSpirvWords);
                code.fragmentSpirv.assign(bundled.fragmentSpirv, bundled.fragmentSpirv + bundled.fragmentSpirvWords);
                code.computeSpirv.assign(bundled.computeSpirv, bundled.computeSpirv + bundled.computeSpirvWords);
                haveCode = (!code.vertexSpirv.empty() && !code.fragmentSpirv.empty()) || !code.computeSpirv.empty();
            }
            if (!haveCode) {
                spdlog::warn("Slang program '{}': the bundle holds no code for {}; compiling from source", program,
                    metal ? "Metal" : "Vulkan");
            }
        }
        if (!haveCode) {
            if (!compileSlangProgramFromSource(device, program, code, variant)) {
                return nullptr;
            }
            // The entry names come from the source the compile read.
            const auto dir = slangSourceDir();
            SourceScan scan;
            std::vector<std::filesystem::path> seen;
            scanSource(dir / "programs" / (program + ".slang"), dir, {}, seen, scan, true);
            for (const auto& [stage, name] : scan.entries) {
                std::string& target = stage == "vertex" ? definition.vshader
                    : stage == "fragment" ? definition.fshader : definition.cshader;
                if (target.empty()) {
                    target = name;
                }
            }
        }

        code.depthOnlyFragment = spirvFragmentWritesNoOutput(code.fragmentSpirv);
        auto shader = device->createShaderFromCode(definition, code);
        if (shader) {
            device->setCachedShader(cacheKey, shader);
            recordLiveSlangShader(device, shader, program, variant, {}, fingerprint);
        } else {
            spdlog::error("Slang program '{}': the device refused the compiled code", program);
        }
        return shader;
    }

    namespace
    {
        /// A shader of `build`'s first entry per stage, cached on the device under `cacheKey`.
        std::shared_ptr<Shader> createShaderFromBuild(GraphicsDevice* device, const std::string& cacheKey,
            const SlangProgramBuild& build, const std::string& name, const ShaderFeatureSet& features = {});

        /// A Slang shader the watcher may recompile: what it was compiled from, and the
        /// fingerprint of those sources when it was (slangSourceFingerprint). Recorded only
        /// while the watcher is on.
        struct LiveSlangShader
        {
            GraphicsDevice* device = nullptr;
            std::weak_ptr<Shader> shader;
            std::string program;   // the program compiled: "app-<name>" for a shader from source
            std::string variant;
            SlangFileOverrides files;
            uint64_t fingerprint = 0;
        };

        std::mutex& liveSlangShadersMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::vector<LiveSlangShader>& liveSlangShaders()
        {
            static std::vector<LiveSlangShader> shaders;
            return shaders;
        }

        void recordLiveSlangShader(GraphicsDevice* device, const std::shared_ptr<Shader>& shader, std::string program,
            std::string variant, SlangFileOverrides files, const uint64_t fingerprint)
        {
            if (!device || !shader || !slangShaderWatchEnabled()) {
                return;
            }
            const std::lock_guard lock(liveSlangShadersMutex());
            liveSlangShaders().push_back(
                {device, shader, std::move(program), std::move(variant), std::move(files), fingerprint});
        }
    }

    std::shared_ptr<Shader> getOrCreateSlangShaderFromSource(GraphicsDevice* device, const std::string& name,
        const std::string& source, const std::string& variant)
    {
        if (!device || source.empty()) {
            return nullptr;
        }
        const std::string cacheKey = "slang-src:" + name + (variant.empty() ? "" : "@" + variant) + ":" +
            hex(fnv1a64(source));
        if (auto cached = device->getCachedShader(cacheKey)) {
            return cached;
        }

        // The source stands in for a program file of its own name: served from memory where
        // the tree would be read, so its includes and imports resolve as an engine program's.
        // The prefix keeps the name clear of every file of the tree.
        const std::string program = "app-" + name;
        SlangProgramBuild build;
        SlangFileOverrides files{{program + ".slang", source}};
        const uint64_t fingerprint = watchFingerprint(program, variant, files);
        if (!compileProgramEntries(device, program, variant, files, build)) {
            return nullptr;
        }
        auto shader = createShaderFromBuild(device, cacheKey, build, name);
        recordLiveSlangShader(device, shader, program, variant, std::move(files), fingerprint);
        return shader;
    }

    std::shared_ptr<Shader> getOrCreateSlangShaderWithFiles(GraphicsDevice* device, const std::string& program,
        const SlangFileOverrides& files, const std::string& variant, const ShaderFeatureSet& features)
    {
        if (files.empty()) {
            return getOrCreateSlangShader(device, program, variant, features);
        }
        if (!device) {
            return nullptr;
        }
        std::string material;
        for (const auto& [name, text] : files) {
            material += name + "\n" + text + "\n";
        }
        std::string cacheKey = "slang:" + program + (variant.empty() ? "" : "@" + variant) + "+" +
            hex(fnv1a64(material));
        if (features != ShaderFeatureSet{}) {
            cacheKey += ":" + hex(features.hash());
        }
        if (auto cached = device->getCachedShader(cacheKey)) {
            return cached;
        }
        const uint64_t fingerprint = watchFingerprint(program, variant, files);
        SlangProgramBuild build;
        if (!compileProgramEntries(device, program, variant, files, build)) {
            return nullptr;
        }
        auto shader = createShaderFromBuild(device, cacheKey, build, program, features);
        recordLiveSlangShader(device, shader, program, variant, files, fingerprint);
        return shader;
    }

    namespace
    {
        std::shared_ptr<Shader> createShaderFromBuild(GraphicsDevice* device, const std::string& cacheKey,
            const SlangProgramBuild& build, const std::string& name, const ShaderFeatureSet& features)
        {
            ShaderDefinition definition;
            definition.name = cacheKey;
            definition.features = features;
            ShaderCode code;
            makeCodeFromBuild(build, definition, code);

            auto shader = device->createShaderFromCode(definition, code);
            if (shader) {
                device->setCachedShader(cacheKey, shader);
            } else {
                spdlog::error("Slang shader '{}': the device refused the compiled code", name);
            }
            return shader;
        }
    }

    bool slangShaderWatchEnabled()
    {
        return envFlag("VISUTWIN_SHADER_WATCH");
    }

    std::filesystem::path slangShaderSourceDir()
    {
        return slangSourceDir();
    }

    uint64_t slangSourceFingerprint(const std::string_view program, const std::string_view variant,
        const SlangFileOverrides& files)
    {
#ifdef VISUTWIN_HAS_SLANG
        const auto dir = slangSourceDir();
        if (dir.empty()) {
            return 0;
        }
        SourceScan scan;
        std::vector<std::filesystem::path> seen;
        scanSource(dir / "programs" / (std::string(program) + ".slang"), dir, files, seen, scan, true);
        if (scan.programText.empty()) {
            return 0;
        }
        return fnv1a64(sourceKeyMaterial(dir, program, variant, scan));
#else
        (void)program;
        (void)variant;
        (void)files;
        return 0;
#endif
    }

    int reloadChangedSlangShaders(GraphicsDevice* device)
    {
        if (!device) {
            return 0;
        }
        int reloaded = 0;
        const std::lock_guard lock(liveSlangShadersMutex());
        auto& live = liveSlangShaders();
        std::erase_if(live, [](const LiveSlangShader& entry) { return entry.shader.expired(); });
        for (auto& entry : live) {
            const auto shader = entry.shader.lock();
            if (entry.device != device || !shader) {
                continue;
            }
            const uint64_t fingerprint = slangSourceFingerprint(entry.program, entry.variant, entry.files);
            if (fingerprint == 0 || fingerprint == entry.fingerprint) {
                continue;
            }
            // A failed compile keeps the code the shader has, and its old fingerprint: the next
            // change to the sources tries again.
            SlangProgramBuild build;
            if (!compileProgramEntries(device, entry.program, entry.variant, entry.files, build)) {
                spdlog::warn("Shader watcher: '{}' did not compile; it keeps its previous code", shader->name());
                continue;
            }
            ShaderDefinition definition;
            ShaderCode code;
            makeCodeFromBuild(build, definition, code);
            if (definition.vshader != shader->vertexEntry() || definition.fshader != shader->fragmentEntry() ||
                definition.cshader != shader->computeEntry()) {
                spdlog::warn("Shader watcher: '{}' changed its entry points; it keeps its previous code until "
                    "the application restarts", shader->name());
                continue;
            }
            if (!device->replaceShaderCode(*shader, code)) {
                spdlog::warn("Shader watcher: the device refused the recompiled '{}'; it keeps its previous code",
                    shader->name());
                continue;
            }
            entry.fingerprint = fingerprint;
            ++reloaded;
            spdlog::info("Shader watcher: reloaded '{}'", shader->name());
        }
        return reloaded;
    }
}
