// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#include "programLibrary.h"

#include <assert.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include "platform/graphics/deviceCache.h"
#include "spdlog/spdlog.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    namespace
    {
        DeviceCache programLibraryDeviceCache;
        std::unordered_map<GraphicsDevice*, std::shared_ptr<ProgramLibrary>> programLibraries;
        
        uint64_t fnv1a64(const std::string& text)
        {
            uint64_t hash = 1469598103934665603ull;
            for (const char c : text) {
                hash ^= static_cast<uint8_t>(c);
                hash *= 1099511628211ull;
            }
            return hash;
        }

        void appendFeatureDefine(std::string& output, const char* name, const bool enabled)
        {
            output += "#define ";
            output += name;
            output += enabled ? " 1\n" : " 0\n";
        }

        struct ShaderChunkRegistry
        {
            std::unordered_map<std::string, std::string> chunkSources;
            std::filesystem::path rootPath;
        };

        std::string readTextFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in.is_open()) {
                return {};
            }

            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }

        std::filesystem::path projectRootFromThisSource()
        {
            auto path = std::filesystem::path(__FILE__).parent_path();
            for (int i = 0; i < 4; ++i) {
                path = path.parent_path();
            }
            return path;
        }

        std::optional<ShaderChunkRegistry> loadShaderChunks()
        {
            const auto sourceRoot = projectRootFromThisSource();
            const auto cwd = std::filesystem::current_path();
            const std::array<std::filesystem::path, 4> chunkRoots = {
                sourceRoot / "engine/shaders/metal/chunks",
                cwd / "engine/shaders/metal/chunks",
                cwd.parent_path() / "engine/shaders/metal/chunks",
                cwd.parent_path().parent_path() / "engine/shaders/metal/chunks"
            };

            for (const auto& root : chunkRoots) {
                ShaderChunkRegistry registry;
                registry.rootPath = root;

                if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root)) {
                    continue;
                }

                for (const auto& entry : std::filesystem::directory_iterator(root)) {
                    if (!entry.is_regular_file() || entry.path().extension() != ".metal") {
                        continue;
                    }

                    const auto chunkName = entry.path().stem().string();
                    const auto chunkSource = readTextFile(entry.path());
                    if (!chunkSource.empty()) {
                        registry.chunkSources[chunkName] = chunkSource;
                    }
                }

                if (!registry.chunkSources.empty()) {
                    spdlog::info("Loaded shader chunks from {}", root.string());
                    return registry;
                }
            }

            return std::nullopt;
        }

        const ShaderChunkRegistry* getShaderChunks()
        {
            static std::optional<ShaderChunkRegistry> chunks = loadShaderChunks();
            return chunks ? &*chunks : nullptr;
        }

        const Material::ParameterValue* getMaterialParameter(const Material* material, std::initializer_list<const char*> names)
        {
            if (!material) {
                return nullptr;
            }
            for (const char* name : names) {
                if (const auto* value = material->parameter(name)) {
                    return value;
                }
            }
            return nullptr;
        }

        bool readParameterBool(const Material::ParameterValue* value, bool& out)
        {
            if (!value) {
                return false;
            }
            if (const auto* v = std::get_if<bool>(value)) {
                out = *v;
                return true;
            }
            if (const auto* v = std::get_if<int32_t>(value)) {
                out = *v != 0;
                return true;
            }
            if (const auto* v = std::get_if<uint32_t>(value)) {
                out = *v != 0u;
                return true;
            }
            if (const auto* v = std::get_if<float>(value)) {
                out = *v != 0.0f;
                return true;
            }
            return false;
        }

        bool readParameterInt(const Material::ParameterValue* value, int& out)
        {
            if (!value) {
                return false;
            }
            if (const auto* v = std::get_if<int32_t>(value)) {
                out = static_cast<int>(*v);
                return true;
            }
            if (const auto* v = std::get_if<uint32_t>(value)) {
                out = static_cast<int>(*v);
                return true;
            }
            if (const auto* v = std::get_if<float>(value)) {
                out = static_cast<int>(*v);
                return true;
            }
            if (const auto* v = std::get_if<bool>(value)) {
                out = *v ? 1 : 0;
                return true;
            }
            return false;
        }

        bool hasTextureParameter(const Material* material, std::initializer_list<const char*> names)
        {
            if (const auto* value = getMaterialParameter(material, names)) {
                if (const auto* texture = std::get_if<Texture*>(value)) {
                    return *texture != nullptr;
                }
            }
            return false;
        }
    }

    ProgramLibrary::ProgramLibrary(const std::shared_ptr<GraphicsDevice>& device, StandardMaterial* standardMaterial) : _device(device)
    {
        (void)standardMaterial;
        // Mirrors upstream program registration model (program -> ordered chunk keys).
        registerProgram("forward", {
            "common",
            "forward-vertex",
            "forward-fragment-head",
            "forward-fragment-lighting",
            "forward-fragment-tail"
        });
        registerProgram("skybox", {
            "common",
            "forward-vertex",
            "forward-fragment-head",
            "forward-fragment-lighting",
            "forward-fragment-tail"
        });
        registerProgram("shadow", {
            "common",
            "shadow-vertex",
            "shadow-fragment"
        });
    }

    void ProgramLibrary::registerProgram(const std::string& name, const std::vector<std::string>& chunkOrder)
    {
        if (name.empty() || chunkOrder.empty()) {
            spdlog::error("ProgramLibrary::registerProgram rejected invalid program registration");
            return;
        }
        _registeredPrograms[name] = chunkOrder;
    }

    bool ProgramLibrary::hasProgram(const std::string& name) const
    {
        return _registeredPrograms.find(name) != _registeredPrograms.end();
    }

    void setProgramLibrary(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<ProgramLibrary>& library)
    {
        assert(library != nullptr && "ProgramLibrary cannot be null");
        programLibraries[device.get()] = library;
        programLibraryDeviceCache.get<ProgramLibrary>(device, [library] {
            return library;
        });
    }

    std::shared_ptr<ProgramLibrary> getProgramLibrary(const std::shared_ptr<GraphicsDevice>& device)
    {
        const auto it = programLibraries.find(device.get());
        return it != programLibraries.end() ? it->second : nullptr;
    }

    ProgramLibrary::ShaderVariantOptions ProgramLibrary::buildForwardVariantOptions(const Material* material,
        const bool transparentPass, const bool dynamicBatch) const
    {
        ShaderVariantOptions options{};
        options.transparentPass = transparentPass;
        options.skybox = material && material->isSkybox();
        options.alphaTest = material && material->alphaMode() == AlphaMode::MASK;

        // StandardMaterial: read properties directly from typed accessors.
        const auto* stdMat = dynamic_cast<const StandardMaterial*>(material);

        CullMode effectiveCullMode = material ? material->cullMode() : CullMode::CULLFACE_BACK;
        if (stdMat) {
            // StandardMaterial stores twoSidedLighting as a separate flag.
            options.doubleSided = effectiveCullMode == CullMode::CULLFACE_NONE || stdMat->twoSidedLighting();
        } else {
            if (int cullModeValue = static_cast<int>(effectiveCullMode);
                readParameterInt(getMaterialParameter(material, {"material_cullMode", "cullMode"}), cullModeValue)) {
                if (cullModeValue >= static_cast<int>(CullMode::CULLFACE_NONE) &&
                    cullModeValue <= static_cast<int>(CullMode::CULLFACE_FRONTANDBACK)) {
                    effectiveCullMode = static_cast<CullMode>(cullModeValue);
                }
            }
            options.doubleSided = effectiveCullMode == CullMode::CULLFACE_NONE;
        }

        if (stdMat) {
            // Prefer StandardMaterial-specific textures, fall back to base Material typed properties.
            options.baseColorMap = (stdMat->diffuseMap() || stdMat->baseColorTexture());
            options.normalMap = (stdMat->normalMap() || stdMat->normalTexture());
            options.metallicRoughnessMap = (stdMat->metalnessMap() || stdMat->metallicRoughnessTexture());
            options.occlusionMap = (stdMat->aoMap() || stdMat->occlusionTexture());
            options.emissiveMap = (stdMat->emissiveMap() || stdMat->emissiveTexture());
        } else {
            options.baseColorMap = (material && material->hasBaseColorTexture()) ||
                hasTextureParameter(material, {"texture_baseColorMap", "texture_diffuseMap", "baseColorTexture"});
            options.normalMap = (material && material->hasNormalTexture()) ||
                hasTextureParameter(material, {"texture_normalMap", "normalTexture"});
            options.metallicRoughnessMap = (material && material->hasMetallicRoughnessTexture()) ||
                hasTextureParameter(material, {"texture_metallicRoughnessMap", "metallicRoughnessTexture"});
            options.occlusionMap = (material && material->hasOcclusionTexture()) ||
                hasTextureParameter(material, {"texture_occlusionMap", "occlusionTexture"});
            options.emissiveMap = (material && material->hasEmissiveTexture()) ||
                hasTextureParameter(material, {"texture_emissiveMap", "emissiveTexture"});
        }

        if (!stdMat) {
            bool skyboxOverride = options.skybox;
            if (readParameterBool(getMaterialParameter(material, {"material_isSkybox", "isSkybox"}), skyboxOverride)) {
                options.skybox = skyboxOverride;
            }
        }

        // Feature flags from StandardMaterial typed properties or shaderVariantKey bits.
        const uint64_t variantBits = material ? material->shaderVariantKey() : 0ull;
        options.shadowMapping = !options.skybox || ((variantBits & (1ull << 10)) != 0ull);
        if (stdMat) {
            options.fog = stdMat->useFog() && !options.skybox;
        } else {
            options.fog = !options.skybox || ((variantBits & (1ull << 11)) != 0ull);
        }
        // parallax from StandardMaterial heightMap or shaderVariantKey.
        if (stdMat) {
            options.parallax = (stdMat->heightMap() != nullptr);
        } else {
            options.parallax = (variantBits & (1ull << 12)) != 0ull;
        }
        // clearcoat from StandardMaterial (clearCoat > 0) or shaderVariantKey.
        if (stdMat) {
            options.clearcoat = stdMat->clearCoat() > 0.0f;
        } else {
            options.clearcoat = (variantBits & (1ull << 13)) != 0ull;
        }
        // anisotropy from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.anisotropy = (stdMat->anisotropy() != 0.0f);
        } else {
            options.anisotropy = (variantBits & (1ull << 14)) != 0ull;
        }
        // sheen from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.sheen = (stdMat->sheenRoughness() > 0.0f ||
                             stdMat->sheenColor() != Color(0.0f, 0.0f, 0.0f, 1.0f));
        } else {
            options.sheen = (variantBits & (1ull << 15)) != 0ull;
        }
        // iridescence from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.iridescence = (stdMat->iridescenceIntensity() > 0.0f);
        } else {
            options.iridescence = (variantBits & (1ull << 16)) != 0ull;
        }
        // transmission from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.transmission = (stdMat->transmissionFactor() > 0.0f);
        } else {
            options.transmission = (variantBits & (1ull << 17)) != 0ull;
        }
        options.lightClustering = _clusteredLightingEnabled || (variantBits & (1ull << 18)) != 0ull;
        options.ssao = _ssaoEnabled || (variantBits & (1ull << 19)) != 0ull;
        options.lightProbes = (variantBits & (1ull << 20)) != 0ull;
        options.vertexColors = (variantBits & (1ull << 21)) != 0ull;
        options.skinning = (variantBits & (1ull << 22)) != 0ull;
        options.morphing = (variantBits & (1ull << 23)) != 0ull;
        // spec-gloss from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.specGloss = (stdMat->specGlossMap() != nullptr);
        } else {
            options.specGloss = (variantBits & (1ull << 24)) != 0ull;
        }
        // Oren-Nayar from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.orenNayar = stdMat->useOrenNayar();
        } else {
            options.orenNayar = (variantBits & (1ull << 25)) != 0ull;
        }
        // detail normals from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.detailNormals = (stdMat->detailNormalMap() != nullptr);
        } else {
            options.detailNormals = (variantBits & (1ull << 26)) != 0ull;
        }
        // displacement from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.displacement = (stdMat->displacementMap() != nullptr);
        } else {
            options.displacement = (variantBits & (1ull << 27)) != 0ull;
        }
        options.atmosphere = (_atmosphereEnabled && options.skybox) || (variantBits & (1ull << 28)) != 0ull;
        options.pointSpotAttenuation = !options.skybox || ((variantBits & (1ull << 29)) != 0ull);
        options.multiLight = !options.skybox || ((variantBits & (1ull << 30)) != 0ull);
        options.instancing = (variantBits & (1ull << 33)) != 0ull;
        options.pointSize = (variantBits & (1ull << 31)) != 0ull;
        options.unlit = (variantBits & (1ull << 32)) != 0ull;

        // shadow catcher flag from StandardMaterial
        if (stdMat) {
            options.shadowCatcher = stdMat->shadowCatcher();
        }

        // when a skybox cubemap is available, compile the
        // skybox shader with the cubemap sampling path instead of envAtlas.
        if (options.skybox && _skyCubemapAvailable) {
            options.skyCubemap = true;
        }

        // DEVIATION: planar reflection is handled at the application level as a script;
        // here it's a material property that triggers a shader variant.
        if (stdMat) {
            options.planarReflection = (stdMat->reflectionMap() != nullptr);
        }

        // depth pass flag set by renderer from camera state.
        // When active, fragment shader outputs distance-from-plane instead of PBR.
        options.planarReflectionDepthPass = _planarReflectionDepthPass;

        // Local light shadows: enabled when any local light has castShadows.
        // Set by the renderer before the draw loop.
        options.localShadows = _localShadowsEnabled && !options.skybox;

        // Omni cubemap shadows: enabled when any omni light has castShadows.
        options.omniShadows = _omniShadowsEnabled && !options.skybox;

        // Area lights: enabled when any area rect light is in the scene.
        // Set by the renderer before the draw loop.
        options.areaLights = _areaLightsEnabled && !options.skybox;

        // Dynamic batching: per-vertex bone index + matrix palette.
        // Set by the renderer from MeshInstance::isDynamicBatch().
        options.dynamicBatch = dynamicBatch;

        return options;
    }

    std::string ProgramLibrary::resolveProgramName(const ShaderVariantOptions& options)
    {
        return options.skybox ? "skybox" : "forward";
    }

    uint64_t ProgramLibrary::makeVariantKey(const std::string& programName, const ShaderVariantOptions& options, const Material* material) const
    {
        (void)material;
        // Build key entirely from resolved ShaderVariantOptions — do NOT fold in
        // the raw material shaderVariantKey, because the options already capture
        // every flag that affects the compiled shader.  Including the raw key was
        // creating spurious unique variants (different materials mapping to the
        // same set of options but different shaderVariantKey values) and hitting
        // the AGX compiled-variants footprint limit.
        uint64_t key = fnv1a64(programName);
        key ^= options.transparentPass ? (1ull << 63) : 0ull;
        key ^= options.skybox ? (1ull << 62) : 0ull;
        key ^= options.baseColorMap ? (1ull << 0) : 0ull;
        key ^= options.normalMap ? (1ull << 1) : 0ull;
        key ^= options.metallicRoughnessMap ? (1ull << 2) : 0ull;
        key ^= options.occlusionMap ? (1ull << 3) : 0ull;
        key ^= options.emissiveMap ? (1ull << 4) : 0ull;
        key ^= options.alphaTest ? (1ull << 5) : 0ull;
        key ^= options.doubleSided ? (1ull << 6) : 0ull;
        key ^= options.shadowMapping ? (1ull << 8) : 0ull;
        key ^= options.fog ? (1ull << 9) : 0ull;
        key ^= options.vertexColors ? (1ull << 10) : 0ull;
        key ^= options.pointSpotAttenuation ? (1ull << 11) : 0ull;
        key ^= options.multiLight ? (1ull << 12) : 0ull;
        key ^= options.envAtlas ? (1ull << 13) : 0ull;
        // Stubbed feature flags (parallax, clearcoat, etc.) are included for
        // correctness but are never true in practice yet, so they don't add
        // extra variants.
        key ^= options.parallax ? (1ull << 14) : 0ull;
        key ^= options.clearcoat ? (1ull << 15) : 0ull;
        key ^= options.anisotropy ? (1ull << 16) : 0ull;
        key ^= options.sheen ? (1ull << 17) : 0ull;
        key ^= options.iridescence ? (1ull << 18) : 0ull;
        key ^= options.transmission ? (1ull << 19) : 0ull;
        key ^= options.lightClustering ? (1ull << 20) : 0ull;
        key ^= options.ssao ? (1ull << 21) : 0ull;
        key ^= options.lightProbes ? (1ull << 22) : 0ull;
        key ^= options.skinning ? (1ull << 23) : 0ull;
        key ^= options.morphing ? (1ull << 24) : 0ull;
        key ^= options.specGloss ? (1ull << 25) : 0ull;
        key ^= options.orenNayar ? (1ull << 26) : 0ull;
        key ^= options.detailNormals ? (1ull << 27) : 0ull;
        key ^= options.displacement ? (1ull << 28) : 0ull;
        key ^= options.atmosphere ? (1ull << 29) : 0ull;
        key ^= options.shadowCatcher ? (1ull << 30) : 0ull;
        key ^= options.skyCubemap ? (1ull << 31) : 0ull;
        key ^= options.instancing ? (1ull << 32) : 0ull;
        key ^= options.planarReflection ? (1ull << 33) : 0ull;
        key ^= options.planarReflectionDepthPass ? (1ull << 34) : 0ull;
        key ^= options.localShadows ? (1ull << 35) : 0ull;
        key ^= options.omniShadows ? (1ull << 36) : 0ull;
        key ^= options.dynamicBatch ? (1ull << 37) : 0ull;
        key ^= options.pointSize ? (1ull << 38) : 0ull;
        key ^= options.areaLights ? (1ull << 40) : 0ull;
        key ^= options.unlit ? (1ull << 39) : 0ull;
        return key;
    }

    std::string ProgramLibrary::composeProgramVariantMetalSource(const std::string& programName, const ShaderVariantOptions& options,
        const std::string& vertexEntry, const std::string& fragmentEntry)
    {
        const auto* chunks = getShaderChunks();
        if (!chunks) {
            spdlog::error("Failed to load shader chunks from engine/shaders/metal/chunks.");
            return {};
        }
        const auto programChunks = _registeredPrograms.find(programName);
        if (programChunks == _registeredPrograms.end() || programChunks->second.empty()) {
            spdlog::error("ProgramLibrary is missing registered chunk order for program '{}'.", programName);
            return {};
        }

        std::string source;
        source.reserve(24 * 1024);

        appendFeatureDefine(source, "VT_FEATURE_SKYBOX", options.skybox);
        appendFeatureDefine(source, "VT_FEATURE_TRANSPARENT_PASS", options.transparentPass);
        appendFeatureDefine(source, "VT_FEATURE_ALPHA_TEST", options.alphaTest);
        appendFeatureDefine(source, "VT_FEATURE_DOUBLE_SIDED", options.doubleSided);
        appendFeatureDefine(source, "VT_FEATURE_BASE_COLOR_MAP", options.baseColorMap);
        appendFeatureDefine(source, "VT_FEATURE_NORMAL_MAP", options.normalMap);
        appendFeatureDefine(source, "VT_FEATURE_METAL_ROUGHNESS_MAP", options.metallicRoughnessMap);
        appendFeatureDefine(source, "VT_FEATURE_OCCLUSION_MAP", options.occlusionMap);
        appendFeatureDefine(source, "VT_FEATURE_EMISSIVE_MAP", options.emissiveMap);
        appendFeatureDefine(source, "VT_FEATURE_ENV_ATLAS", options.envAtlas);

        appendFeatureDefine(source, "VT_FEATURE_SHADOWS", options.shadowMapping);
        appendFeatureDefine(source, "VT_FEATURE_FOG", options.fog);
        appendFeatureDefine(source, "VT_FEATURE_PARALLAX", options.parallax);
        appendFeatureDefine(source, "VT_FEATURE_CLEARCOAT", options.clearcoat);
        appendFeatureDefine(source, "VT_FEATURE_ANISOTROPY", options.anisotropy);
        appendFeatureDefine(source, "VT_FEATURE_SHEEN", options.sheen);
        appendFeatureDefine(source, "VT_FEATURE_IRIDESCENCE", options.iridescence);
        appendFeatureDefine(source, "VT_FEATURE_TRANSMISSION", options.transmission);
        appendFeatureDefine(source, "VT_FEATURE_LIGHT_CLUSTERING", options.lightClustering);
        appendFeatureDefine(source, "VT_FEATURE_SSAO", options.ssao);
        appendFeatureDefine(source, "VT_FEATURE_LIGHT_PROBES", options.lightProbes);
        appendFeatureDefine(source, "VT_FEATURE_VERTEX_COLORS", options.vertexColors);
        appendFeatureDefine(source, "VT_FEATURE_SKINNING", options.skinning);
        appendFeatureDefine(source, "VT_FEATURE_MORPHS", options.morphing);
        appendFeatureDefine(source, "VT_FEATURE_SPEC_GLOSS", options.specGloss);
        appendFeatureDefine(source, "VT_FEATURE_OREN_NAYAR", options.orenNayar);
        appendFeatureDefine(source, "VT_FEATURE_DETAIL_NORMALS", options.detailNormals);
        appendFeatureDefine(source, "VT_FEATURE_DISPLACEMENT", options.displacement);
        appendFeatureDefine(source, "VT_FEATURE_ATMOSPHERE", options.atmosphere);
        appendFeatureDefine(source, "VT_FEATURE_AREA_LIGHTS", options.areaLights);
        appendFeatureDefine(source, "VT_FEATURE_POINT_SPOT_ATTENUATION", options.pointSpotAttenuation);
        appendFeatureDefine(source, "VT_FEATURE_MULTI_LIGHT", options.multiLight);
        appendFeatureDefine(source, "VT_FEATURE_SHADOW_CATCHER", options.shadowCatcher);
        appendFeatureDefine(source, "VT_FEATURE_SKY_CUBEMAP", options.skyCubemap);
        appendFeatureDefine(source, "VT_FEATURE_SURFACE_LIC", options.surfaceLIC);
        appendFeatureDefine(source, "VT_FEATURE_INSTANCING", options.instancing);
        appendFeatureDefine(source, "VT_FEATURE_PLANAR_REFLECTION", options.planarReflection);
        appendFeatureDefine(source, "VT_FEATURE_PLANAR_REFLECTION_DEPTH_PASS", options.planarReflectionDepthPass);
        appendFeatureDefine(source, "VT_FEATURE_LOCAL_SHADOWS", options.localShadows);
        appendFeatureDefine(source, "VT_FEATURE_OMNI_SHADOWS", options.omniShadows);
        appendFeatureDefine(source, "VT_FEATURE_DYNAMIC_BATCH", options.dynamicBatch);
        appendFeatureDefine(source, "VT_FEATURE_POINT_SIZE", options.pointSize);
        appendFeatureDefine(source, "VT_FEATURE_UNLIT", options.unlit);
        // VT_FEATURE_HDR_PASS is not emitted as a compile-time define.
        // It is passed as a runtime uniform bit in LightingData.flagsAndPad
        // to avoid doubling the number of compiled shader variants.

        source += "\n#define VT_VERTEX_ENTRY ";
        source += vertexEntry;
        source += "\n#define VT_FRAGMENT_ENTRY ";
        source += fragmentEntry;
        source += "\n\n";

        for (const auto& chunkName : programChunks->second) {
            const auto chunkIt = chunks->chunkSources.find(chunkName);
            if (chunkIt == chunks->chunkSources.end()) {
                spdlog::error("ProgramLibrary chunk '{}' is missing in '{}'.",
                    chunkName, chunks->rootPath.string());
                return {};
            }
            source += chunkIt->second;
            source += "\n";
        }

        return source;
    }

    std::shared_ptr<Shader> ProgramLibrary::buildForwardShaderVariant(const std::string& programName,
        const ShaderVariantOptions& options, const uint64_t variantKey)
    {
        ShaderDefinition definition;
        definition.name = "program-" + programName;
        definition.name += options.transparentPass ? "-transparent" : "-opaque";
        definition.name += "-" + std::to_string(variantKey);
        const auto entryPrefix = programName == "shadow" ? "pcShadow" : "pcForward";
        definition.vshader = entryPrefix + std::string("VS_") + std::to_string(variantKey);
        definition.fshader = entryPrefix + std::string("FS_") + std::to_string(variantKey);
        const std::string sourceCode = composeProgramVariantMetalSource(programName, options, definition.vshader, definition.fshader);
        if (sourceCode.empty()) {
            return nullptr;
        }
        return createShader(_device.get(), definition, sourceCode);
    }

    std::shared_ptr<Shader> ProgramLibrary::getForwardShader(const Material* material, const bool transparentPass,
        const bool dynamicBatch)
    {
        if (!_device) {
            return nullptr;
        }

        const ShaderVariantOptions options = buildForwardVariantOptions(material, transparentPass, dynamicBatch);
        const std::string programName = resolveProgramName(options);
        if (!hasProgram(programName)) {
            spdlog::error("ProgramLibrary has no registered program '{}'.", programName);
            return nullptr;
        }
        const uint64_t key = makeVariantKey(programName, options, material);

        const auto cached = _forwardShaderCache.find(key);
        if (cached != _forwardShaderCache.end()) {
            return cached->second;
        }

        auto warnFeature = [&](const char* featureName, const bool enabled) {
            if (!enabled) {
                return;
            }
            if (_warnedFeatureFlags.insert(featureName).second) {
                spdlog::warn("Shader variant feature '{}' enabled but only chunk scaffolding is present. Full shader chunk port is pending.",
                    featureName);
            }
        };

        // parallax: fully implemented — no warning needed.
        // clearcoat: fully implemented — no warning needed.
        // anisotropy: fully implemented — no warning needed.
        // sheen: fully implemented — no warning needed.
        // iridescence: fully implemented — no warning needed.
        // transmission: fully implemented — no warning needed.
        // lightClustering: fully implemented — no warning needed.
        // ssao: fully implemented — no warning needed.
        warnFeature("lightProbes", options.lightProbes);
        // vertexColors: fully implemented — no warning needed.
        warnFeature("skinning", options.skinning);
        warnFeature("morphing", options.morphing);
        warnFeature("specGloss", options.specGloss);
        warnFeature("orenNayar", options.orenNayar);
        warnFeature("detailNormals", options.detailNormals);
        warnFeature("displacement", options.displacement);
        // atmosphere: fully implemented — no warning needed.

        auto shader = buildForwardShaderVariant(programName, options, key);
        if (!shader) {
            spdlog::error("Failed to build shader variant '{}' (key={:#x}, localShadows={}, shadows={}, envAtlas={})",
                programName, key, options.localShadows, options.shadowMapping, options.envAtlas);
        }
        _forwardShaderCache[key] = shader;
        return shader;
    }

    std::shared_ptr<Shader> ProgramLibrary::getShadowShader(const bool dynamicBatch)
    {
        if (!_device || !hasProgram("shadow")) {
            return nullptr;
        }

        ShaderVariantOptions options{};
        options.skybox = false;
        options.transparentPass = false;
        options.alphaTest = false;
        options.doubleSided = false;
        options.shadowMapping = true;
        options.fog = false;
        options.multiLight = false;
        options.dynamicBatch = dynamicBatch;

        const uint64_t key = makeVariantKey("shadow", options, nullptr);
        const auto cached = _forwardShaderCache.find(key);
        if (cached != _forwardShaderCache.end()) {
            return cached->second;
        }

        auto shader = buildForwardShaderVariant("shadow", options, key);
        _forwardShaderCache[key] = shader;
        return shader;
    }

    void ProgramLibrary::bindMaterial(const std::shared_ptr<GraphicsDevice>& device, const Material* material,
        const bool transparentPass, const bool dynamicBatch)
    {
        if (!device) {
            return;
        }

        auto shader = material ? material->shaderOverride() : nullptr;
        if (!shader) {
            shader = getForwardShader(material, transparentPass, dynamicBatch);
        }

        auto blendState = material ? material->blendState() : nullptr;
        auto depthState = material ? material->depthState() : nullptr;

        if (shader) {
            device->setShader(shader);
        }
        if (blendState) {
            device->setBlendState(blendState);
        }
        if (depthState) {
            device->setDepthState(depthState);
        }
        device->setMaterial(material);
    }
}
