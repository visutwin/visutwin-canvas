// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
#include "slangShaders.h"

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

        /// The entry point names a program declares, by stage.
        struct EntryPoints
        {
            std::string vertex;
            std::string fragment;
            std::string compute;
        };

        EntryPoints findEntryPoints(const std::string& source)
        {
            // A delimited raw string: the pattern itself contains `)"`.
            static const std::regex pattern(R"re(\[shader\("(vertex|fragment|compute)"\)\]\s*[\w<>:]+\s+(\w+)\s*\()re");
            EntryPoints found;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), pattern); it != std::sregex_iterator(); ++it) {
                const std::string stage = (*it)[1];
                const std::string name = (*it)[2];
                if (stage == "vertex") found.vertex = name;
                else if (stage == "fragment") found.fragment = name;
                else found.compute = name;
            }
            return found;
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

    bool findBundledSlangProgram(const std::string_view program, SlangBundledProgram& out)
    {
#ifdef VISUTWIN_HAS_SLANG
        const auto* entry = slang_generated::findProgram(program);
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
        return true;
#else
        (void)program;
        (void)out;
        return false;
#endif
    }

    bool compileSlangProgramFromSource(GraphicsDevice* device, const std::string_view program, ShaderCode& code)
    {
#ifndef VISUTWIN_HAS_SLANG
        (void)device;
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
        const std::string source = readTextFile(path);
        if (source.empty()) {
            spdlog::error("Slang program '{}': {} is missing or empty", program, path.string());
            return false;
        }
        const EntryPoints entries = findEntryPoints(source);
        const bool graphics = !entries.vertex.empty() && !entries.fragment.empty();
        if (!graphics && entries.compute.empty()) {
            spdlog::error("Slang program '{}': no [shader(...)] entry points in {}", program, path.string());
            return false;
        }

        const bool metal = device->shaderLanguage() == ShaderLanguage::Msl;
        SlangCompileRequest request;
        request.moduleName = std::string(program);
        request.source = source;
        if (graphics) {
            request.entryPoints = {entries.vertex, entries.fragment};
        } else {
            request.entryPoints = {entries.compute};
        }
        request.target = metal ? SlangTarget::Msl : SlangTarget::Spirv;
        request.searchPaths = {(dir / "modules").string()};
        // The generated bindings.slang is not in the source tree: the bundle carries its text.
        request.virtualFiles = {{"bindings.slang", slang_generated::kBindingsSlang}};

        // Everything the output is a function of, for the device's persistent cache: the
        // Slang version, the target, the program and every module file (an import closure
        // is not reported, and the tree is small), and the bindings text.
        std::string cacheKey = std::string("slang ") + SlangCompiler::version() + (metal ? " metal\n" : " spirv\n");
        cacheKey += "program " + std::string(program) + "\n" + source + "\n";
        for (const auto& entry : std::filesystem::directory_iterator(dir / "modules")) {
            if (entry.is_regular_file() && entry.path().extension() == ".slang") {
                cacheKey += "module " + entry.path().filename().string() + "\n" + readTextFile(entry.path()) + "\n";
            }
        }
        cacheKey += slang_generated::kBindingsSlang;
        const std::string cacheName = std::string(program) + "-" + hex(fnv1a64(cacheKey));

        const ShaderDiskCache* cache = device->shaderDiskCache();
        if (cache && !metal) {
            const auto load = [&](const char* kind, std::vector<uint32_t>& words) {
                const auto stored = cache->load(kind, cacheName);
                if (!stored || stored->empty() || stored->size() % sizeof(uint32_t) != 0) {
                    return false;
                }
                words.resize(stored->size() / sizeof(uint32_t));
                std::memcpy(words.data(), stored->data(), stored->size());
                return true;
            };
            if (graphics ? (load("slang-vert", code.vertexSpirv) && load("slang-frag", code.fragmentSpirv))
                         : load("slang-comp", code.computeSpirv)) {
                code.specializeFeatures = true;
                return true;
            }
        }

        const auto result = SlangCompiler::compile(request);
        if (!result.ok) {
            spdlog::error("Slang program '{}' failed to compile for {}:\n{}", program, metal ? "Metal" : "Vulkan",
                result.diagnostics);
            return false;
        }
        spdlog::info("Slang program '{}' compiled from {} for {} in {:.1f} ms", program, path.string(),
            metal ? "Metal" : "Vulkan", result.seconds * 1000.0);
        code.specializeFeatures = true;
        if (metal) {
            code.metalSource = result.programText();
            return true;
        }
        if (graphics) {
            code.vertexSpirv = result.spirv(0);
            code.fragmentSpirv = result.spirv(1);
        } else {
            code.computeSpirv = result.spirv(0);
        }
        if (cache) {
            const auto store = [&](const char* kind, const std::vector<uint32_t>& words) {
                cache->store(kind, cacheName,
                    {reinterpret_cast<const uint8_t*>(words.data()), words.size() * sizeof(uint32_t)});
            };
            if (graphics) {
                store("slang-vert", code.vertexSpirv);
                store("slang-frag", code.fragmentSpirv);
            } else {
                store("slang-comp", code.computeSpirv);
            }
        }
        return true;
#endif
    }

    std::shared_ptr<Shader> getOrCreateSlangShader(GraphicsDevice* device, const std::string& program,
        const ShaderFeatureSet& features)
    {
        if (!device) {
            return nullptr;
        }
        std::string cacheKey = "slang:" + program;
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
        const bool inBundle = findBundledSlangProgram(program, bundled);
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
            if (!compileSlangProgramFromSource(device, program, code)) {
                return nullptr;
            }
            // The entry names come from the source the compile read.
            const auto dir = slangSourceDir();
            const EntryPoints entries = findEntryPoints(readTextFile(dir / "programs" / (program + ".slang")));
            definition.vshader = entries.vertex;
            definition.fshader = entries.fragment;
            definition.cshader = entries.compute;
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
