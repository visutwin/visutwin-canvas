// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 25.09.2026
//
// Shader composition contracts that only ever broke silently:
//
//  - The forward chunk registry is the files the forward and shadow programs #include:
//    every chunk they include is in it, and every chunk in it is included by one of them
//    (an override of a chunk nobody includes would change nothing, and say nothing).
//  - A chunk override resolves material > registry > default, as the files the program is
//    compiled with.
//  - A variant's key changes exactly when an override that feeds it changes, so a new
//    override compiles a new variant and an unchanged one reuses the cache.
//  - useLighting(false) keeps the lit pipeline with no lights (VT_FEATURE_NO_LIGHTS, no
//    clustered lights), setUnlit is the fully unlit path, and vertex colours are linear
//    unless vertexColorGamma (or variant bit 35) says otherwise.
//
// CPU only: nothing is compiled.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/programLibrary.h"
#include "support/check.h"
#include "support/stubDevice.h"

namespace visutwin::canvas
{
    // The seam ProgramLibrary befriends for this test.
    struct ProgramLibraryTestAccess
    {
        static SlangFileOverrides overrides(const ProgramLibrary& library, const Material* material)
        {
            return library.chunkOverrides(material);
        }
        static ProgramLibrary::VariantKey key(const ProgramLibrary& library, const Material* material)
        {
            const auto options = library.buildForwardVariantOptions(material, false);
            return library.makeVariantKey(ProgramLibrary::resolveProgramName(options), options, material);
        }
    };
}

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // The forward chunks a file includes, transitively ("forward/<name>.slang").
    void forwardIncludes(const std::filesystem::path& slangRoot, const std::filesystem::path& file,
        std::set<std::string>& names)
    {
        std::ifstream in(file);
        const std::regex include(R"re(#include\s+"forward/([A-Za-z0-9_-]+)\.slang")re");
        std::string line;
        while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_search(line, m, include) && names.insert(m[1]).second) {
                forwardIncludes(slangRoot, slangRoot / "forward" / (std::string(m[1]) + ".slang"), names);
            }
        }
    }

    const std::string* overrideFor(const SlangFileOverrides& overrides, const std::string& file)
    {
        for (const auto& [name, source] : overrides) {
            if (name == file) {
                return &source;
            }
        }
        return nullptr;
    }
}

int main()
{
    std::cout << std::unitbuf;

    auto device = std::make_shared<StubGraphicsDevice>();
    ProgramLibrary msl(device);

    std::cout << "the registry is the programs' chunks\n";
    {
        const auto slangRoot = msl.chunks().rootPath().parent_path();
        std::set<std::string> included;
        for (const char* program : {"forward", "shadow"}) {
            forwardIncludes(slangRoot, slangRoot / "programs" / (std::string(program) + ".slang"), included);
        }
        const auto names = msl.chunks().names();
        check(!names.empty() && !included.empty(), "the chunks and the programs are found (" + slangRoot.string() + ")");
        bool allIncluded = true;
        for (const auto& name : names) {
            if (!included.count(name)) {
                allIncluded = false;
                std::cout << "        '" << name << "' is in the registry but no program includes it\n";
            }
        }
        check(allIncluded, "every chunk in the registry is included by the forward or shadow program");
        bool allKnown = true;
        for (const auto& name : included) {
            if (!msl.chunks().has(name)) {
                allKnown = false;
                std::cout << "        '" << name << "' is included but not in the registry\n";
            }
        }
        check(allKnown, "and every chunk they include is in the registry");
        check(included.count("forward-vertex-local") == 1, "including the local-space vertex hook");
    }

    std::cout << "\noverride precedence\n";
    {
        check(ProgramLibraryTestAccess::overrides(msl, nullptr).empty(), "no override, no file replaced");
        msl.chunks().set("common-dither", "// REGISTRY-OVERRIDE\n");
        StandardMaterial material;
        material.setShaderChunk("common-dither", "// MATERIAL-OVERRIDE\n");
        const auto withMaterial = ProgramLibraryTestAccess::overrides(msl, &material);
        const auto withRegistry = ProgramLibraryTestAccess::overrides(msl, nullptr);
        const auto* m = overrideFor(withMaterial, "common-dither.slang");
        const auto* r = overrideFor(withRegistry, "common-dither.slang");
        check(m && *m == "// MATERIAL-OVERRIDE\n", "a material override beats the registry's");
        check(r && *r == "// REGISTRY-OVERRIDE\n", "the registry's replaces the default file");
        msl.chunks().remove("common-dither");
        check(ProgramLibraryTestAccess::overrides(msl, nullptr).empty(), "removing it gives the default back");
    }

    std::cout << "\nvariant keys\n";
    {
        StandardMaterial a;
        StandardMaterial b;
        const auto base = ProgramLibraryTestAccess::key(msl, &a);
        check(base == ProgramLibraryTestAccess::key(msl, &b), "two plain materials share a variant");
        b.setShaderChunk("common-utils", "// one\n");
        const auto one = ProgramLibraryTestAccess::key(msl, &b);
        check(!(one == base), "a material override makes a new variant");
        b.setShaderChunk("common-utils", "// two\n");
        const auto two = ProgramLibraryTestAccess::key(msl, &b);
        check(!(two == one), "and a DIFFERENT override another");
        msl.chunks().set("common-utils", "// registry\n");
        const auto registry = ProgramLibraryTestAccess::key(msl, &a);
        check(!(registry == base), "a registry override re-keys every material");
        msl.chunks().remove("common-utils");
        check(ProgramLibraryTestAccess::key(msl, &a) == base, "and removing it returns to the old key");

        // useTonemap off (UI materials) is a variant of its own, and the default is on.
        check(a.useTonemap() && !base.features.test(ShaderFeature::NoTonemap),
            "a default material is tone mapped");
        a.setUseTonemap(false);
        const auto untonemapped = ProgramLibraryTestAccess::key(msl, &a);
        check(untonemapped.features.test(ShaderFeature::NoTonemap) && !(untonemapped == base),
            "useTonemap(false) compiles VT_FEATURE_NO_TONEMAP");
        a.setUseTonemap(true);
        check(ProgramLibraryTestAccess::key(msl, &a) == base, "and turning it back on returns to the old key");
    }

    std::cout << "\nlighting off, unlit and vertex-colour gamma\n";
    {
        // Under clustered lighting and area lights, so that dropping them is seen.
        msl.setClusteredLightingEnabled(true);
        msl.setAreaLightsEnabled(true);
        msl.setLocalShadowsEnabled(true);

        StandardMaterial a;
        const auto base = ProgramLibraryTestAccess::key(msl, &a);
        check(!base.features.test(ShaderFeature::NoLights) && !base.features.test(ShaderFeature::Unlit) &&
              !base.features.test(ShaderFeature::VertexColorGamma) &&
              base.features.test(ShaderFeature::LightClustering),
            "a default material is lit, clustered, and reads vertex colours as linear");

        // useLighting off keeps the lit pipeline and drops only the lights.
        a.setUseLighting(false);
        const auto noLights = ProgramLibraryTestAccess::key(msl, &a);
        check(noLights.features.test(ShaderFeature::NoLights) && !noLights.features.test(ShaderFeature::Unlit),
            "useLighting(false) compiles VT_FEATURE_NO_LIGHTS, not the unlit path");
        check(!noLights.features.test(ShaderFeature::LightClustering) &&
              !noLights.features.test(ShaderFeature::AreaLights) &&
              !noLights.features.test(ShaderFeature::LocalShadows),
            "and takes no clustered lights, area lights or local shadows");
        check(noLights.features.test(ShaderFeature::Fog) == base.features.test(ShaderFeature::Fog),
            "while its fog is untouched");
        a.setUseLighting(true);
        check(ProgramLibraryTestAccess::key(msl, &a) == base, "turning lighting back on returns to the old key");

        // The fully unlit output is its own switch.
        a.setUnlit(true);
        const auto unlit = ProgramLibraryTestAccess::key(msl, &a);
        check(unlit.features.test(ShaderFeature::Unlit) && !unlit.features.test(ShaderFeature::NoLights),
            "setUnlit(true) compiles VT_FEATURE_UNLIT");
        a.setUnlit(false);
        check(ProgramLibraryTestAccess::key(msl, &a) == base, "and turning it off returns to the old key");

        // Gamma-encoded vertex colours, from a StandardMaterial or from variant bit 35.
        a.setVertexColorGamma(true);
        const auto gamma = ProgramLibraryTestAccess::key(msl, &a);
        check(gamma.features.test(ShaderFeature::VertexColorGamma) && !(gamma == base),
            "setVertexColorGamma(true) compiles VT_FEATURE_VERTEX_COLOR_GAMMA");
        a.setVertexColorGamma(false);
        check(ProgramLibraryTestAccess::key(msl, &a) == base, "and turning it off returns to the old key");

        Material generic;
        check(!ProgramLibraryTestAccess::key(msl, &generic).features.test(ShaderFeature::VertexColorGamma),
            "a generic material reads vertex colours as linear");
        generic.setShaderVariantKey(1ull << 35);
        check(ProgramLibraryTestAccess::key(msl, &generic).features.test(ShaderFeature::VertexColorGamma),
            "and variant bit 35 makes them gamma encoded");

        // Direct specular is Blinn-Phong unless GGX is asked for, and anisotropy is a GGX
        // lobe: without the opt-in it compiles nothing.
        check(!base.features.test(ShaderFeature::GgxSpecular),
            "a default material's light specular is Blinn-Phong, not GGX");
        a.setAnisotropy(0.5f);
        check(ProgramLibraryTestAccess::key(msl, &a) == base,
            "anisotropy without enableGGXSpecular changes no variant");
        a.setEnableGGXSpecular(true);
        const auto ggx = ProgramLibraryTestAccess::key(msl, &a);
        check(ggx.features.test(ShaderFeature::GgxSpecular) && ggx.features.test(ShaderFeature::Anisotropy),
            "setEnableGGXSpecular(true) compiles VT_FEATURE_GGX_SPECULAR, and anisotropy with it");
        a.setAnisotropy(0.0f);
        a.setEnableGGXSpecular(false);
        check(ProgramLibraryTestAccess::key(msl, &a) == base, "and turning both off returns to the old key");
        generic.setShaderVariantKey(1ull << 36);
        check(ProgramLibraryTestAccess::key(msl, &generic).features.test(ShaderFeature::GgxSpecular),
            "variant bit 36 asks a generic material for GGX");

        msl.setClusteredLightingEnabled(false);
        msl.setAreaLightsEnabled(false);
        msl.setLocalShadowsEnabled(false);
    }

    return finish("shader composition");
}
