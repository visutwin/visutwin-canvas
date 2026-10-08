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

        bool wantsRuntimeCompile()
        {
            const char* value = std::getenv("VISUTWIN_SLANG_RUNTIME");
            return value && *value && std::strcmp(value, "0") != 0;
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
            SlangBundledProgram::Entry copy{e.stage, e.name, e.spirv, e.spirvWords, {}, e.pushConstantBytes};
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
            // Slang version, the target, the program and every file it includes, every module
            // (an import closure is not reported, and the tree is small), and the bindings.
            std::string cacheKey = std::string("slang ") + SlangCompiler::version() + (metal ? " metal\n" : " spirv\n");
            cacheKey += "program " + std::string(program) + " variant " + std::string(variant) + "\n" + scan.keyMaterial;
            for (const auto& entry : std::filesystem::directory_iterator(dir / "modules")) {
                if (entry.is_regular_file() && entry.path().extension() == ".slang") {
                    cacheKey += "module " + entry.path().filename().string() + "\n" + readTextFile(entry.path()) + "\n";
                }
            }
            cacheKey += slang_generated::kBindingsSlang;
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
            spdlog::info("Slang program '{}'{}{} compiled from {} for {} in {:.1f} ms", program,
                variant.empty() ? "" : " variant ", variant, path.string(), metal ? "Metal" : "Vulkan",
                result.seconds * 1000.0);
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
                SlangProgramBuild::Entry e{std::string(entry.stage), std::string(entry.name), {}};
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
            if (metal) {
                // The precompiled library, so the first use costs a load, not an MSL
                // compile; MetalShader takes the source only when no library is given.
                if (bundled.metalLibrary && bundled.metalLibraryBytes > 0) {
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

        auto shader = device->createShaderFromCode(definition, code);
        if (shader) {
            device->setCachedShader(cacheKey, shader);
        } else {
            spdlog::error("Slang program '{}': the device refused the compiled code", program);
        }
        return shader;
    }
}
