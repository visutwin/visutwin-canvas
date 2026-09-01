// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#include "programLibrary.h"

#include "shaderChunks.h"

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

    ProgramLibrary::ProgramLibrary(const std::shared_ptr<GraphicsDevice>& device)
        : _device(device),
          _chunks(device ? device->shaderLanguage() : ShaderLanguage::Msl)
    {
        // Mirrors upstream program registration model (program -> ordered chunk keys).
        // Chunks are named micro-sections; any of them can be overridden globally via
        // chunks().set() or per material via Material::setShaderChunk().
        //
        // The two languages register DIFFERENT orders because the shaders are built
        // differently: MSL composes one translation unit carrying both stages, while
        // the Vulkan tree is the fragment stage only (its vertex stage is a family of
        // prebuilt modules selected by feature). Chunk names are shared, so an
        // override written against a name lands on whichever backend is running.
        if (_chunks.language() == ShaderLanguage::Glsl) {
            registerGlslPrograms();
            return;
        }
        registerProgram("forward", {
            "common-structs",
            "common-utils",
            "common-tonemap",
            "common-falloff",
            "common-dither",
            "common-ltc",
            "common-shadow-pcf",
            "common-shadow-vsm",
            "common-shadow-pcss",
            "common-cookie",
            "common-brdf",
            "common-sheen",
            "common-iridescence",
            "common-atmosphere",
            "common-parallax",
            "forward-vertex",
            "forward-fragment-head",
            "forward-fragment-surface",
            "forward-fragment-lights",
            "forward-fragment-clustered",
            "forward-fragment-ambient",
            "forward-fragment-emissive",
            "forward-fragment-tail"
        });
        registerProgram("skybox", {
            "common-structs",
            "common-utils",
            "common-tonemap",
            "common-falloff",
            "common-dither",
            "common-ltc",
            "common-shadow-pcf",
            "common-shadow-vsm",
            "common-shadow-pcss",
            "common-cookie",
            "common-brdf",
            "common-sheen",
            "common-iridescence",
            "common-atmosphere",
            "common-parallax",
            "forward-vertex",
            "forward-fragment-head",
            "forward-fragment-surface",
            "forward-fragment-lights",
            "forward-fragment-clustered",
            "forward-fragment-ambient",
            "forward-fragment-emissive",
            "forward-fragment-tail"
        });
        registerProgram("shadow", {
            "common-structs",
            "common-utils",
            "common-tonemap",
            "common-falloff",
            "common-dither",
            "common-ltc",
            "common-shadow-pcf",
            "common-shadow-vsm",
            "common-shadow-pcss",
            "common-cookie",
            "common-brdf",
            "common-sheen",
            "common-iridescence",
            "common-atmosphere",
            "common-parallax",
            "shadow-vertex",
            "shadow-fragment"
        });
    }

    void ProgramLibrary::registerGlslPrograms()
    {
        // Fragment-stage chunk order for Vulkan. MUST stay in step with the
        // #include order in engine/shaders/vulkan/forward.frag — that file is the
        // build-time composition of these same chunks, and this is the runtime one.
        // "skybox" shares the order: one fragment shader serves both, gated by
        // VT_FEATURE_SKYBOX.
        const std::vector<std::string> forwardChunks = {
            "forward-fragment-head",
            "common-dither",
            "common-parallax",
            "common-shadow-pcss",
            "common-shadow-vsm",
            "common-cookie",
            "common-utils",
            "common-tonemap",
            "common-material-flags",
            "common-ltc",
            "common-brdf",
            "common-atmosphere",
            "forward-fragment-surface",
            "forward-fragment-lights",
            "forward-fragment-clustered",
            "forward-fragment-ambient",
            "forward-fragment-emissive",
            "forward-fragment-tail"
        };
        registerProgram("forward", forwardChunks);
        registerProgram("skybox", forwardChunks);
        // No "shadow": the Vulkan shadow pass is depth-only for PCF and uses the
        // standalone shadow_vsm_moments.frag for VSM, neither of which is composed
        // from chunks. Overriding a shadow chunk is reported by composeGlsl().
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
        const bool transparentPass, const bool dynamicBatch, const bool skinning, const bool morphing,
        const bool instancing, const bool instancingColor) const
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
        // Dynamic grab-pass refraction: StandardMaterial opt-in, requires transmission.
        options.dynamicRefraction = options.transmission && stdMat && stdMat->useDynamicRefraction();
        options.ssr = stdMat && stdMat->useScreenSpaceReflection();
        // Opacity dither: Bayer8 dithered transparency in the opaque pass.
        options.opacityDither = stdMat && stdMat->opacityDither();
        options.envAtlas = _envAtlasEnabled;
        options.reflectionProbe = _reflectionProbeEnabled;
        options.lightClustering = _clusteredLightingEnabled || (variantBits & (1ull << 18)) != 0ull;
        options.ssao = _ssaoEnabled || (variantBits & (1ull << 19)) != 0ull;
        options.lightProbes = _lightProbesEnabled || (variantBits & (1ull << 20)) != 0ull;
        // lightmap from StandardMaterial or shaderVariantKey bit 34.
        if (stdMat) {
            options.lightmap = (stdMat->lightMap() != nullptr);
        } else {
            options.lightmap = (variantBits & (1ull << 34)) != 0ull;
        }
        // Vertex colors stay an explicit opt-in (bit 21) because the material cannot
        // see whether the mesh even carries a color stream — but asking for
        // emissiveVertexColor is that opt-in, and would otherwise silently do nothing.
        options.vertexColors = (variantBits & (1ull << 21)) != 0ull ||
            (stdMat && stdMat->emissiveVertexColor());
        // Skinning/morphing are per-draw flags set by the renderer from
        // MeshInstance::skinInstance()/morphInstance(); the variant-key bits
        // remain as a material-level override.
        options.skinning = skinning || (variantBits & (1ull << 22)) != 0ull;
        options.morphing = morphing || (variantBits & (1ull << 23)) != 0ull;
        // spec-gloss from StandardMaterial or shaderVariantKey.
        if (stdMat) {
            options.specGloss = stdMat->useSpecGloss() || (stdMat->specGlossMap() != nullptr);
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
        // Instancing follows the draw: a mesh instance with a per-instance buffer gets the
        // instanced vertex stage (upstream infers it from MeshInstance::setInstancing the same
        // way). Variant bit 33 stays honoured so materials that opted in explicitly keep working;
        // for those the per-instance color is assumed, since the 80-byte layout was the only one
        // the engine supported when that bit was the sole way to request instancing.
        const bool materialInstancing = (variantBits & (1ull << 33)) != 0ull;
        options.instancing = instancing || materialInstancing;
        options.instancingColor = instancing ? instancingColor : materialInstancing;
        options.pointSize = (variantBits & (1ull << 31)) != 0ull;
        // unlit: shaderVariantKey bit 32 (used by glb-parser for KHR_materials_unlit assets) OR
        // a StandardMaterial with lighting disabled (decals, debug visualizers, holograms).
        options.unlit = (variantBits & (1ull << 32)) != 0ull ||
                        (stdMat && !stdMat->useLighting());

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
        options.lightmapBake = _lightmapBakePass;
        options.lightmapBakeAccum = _lightmapBakeAccumulate;

        // Debug surface-quantity output, set by the renderer from the camera's debugShaderPass.
        options.debugPass = _debugPassEnabled;

        // Local light shadows: enabled when any local light has castShadows.
        // Set by the renderer before the draw loop.
        options.localShadows = _localShadowsEnabled && !options.skybox;

        // Omni cubemap shadows: enabled when any omni light has castShadows.
        options.omniShadows = _omniShadowsEnabled && !options.skybox;

        // Light cookies: the projected texture masking a spot or omni light's color.
        // Set by the renderer before the draw loop.
        options.cookie2D = _cookie2DEnabled && !options.skybox;
        options.cookieCube = _cookieCubeEnabled && !options.skybox;

        // Directional EVSM_16F: forward shader samples moments texture via Chebyshev.
        options.vsmShadows = _vsmShadowsEnabled && !options.skybox;
        options.pcssShadows = _pcssShadowsEnabled;

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

    ShaderFeatureSet ProgramLibrary::makeFeatureSet(
        const ShaderVariantOptions& options)
    {
        ShaderFeatureSet features;
        const auto set = [&features](const ShaderFeature feature, const bool enabled) {
            if (enabled) features.set(feature);
        };
        set(ShaderFeature::TransparentPass, options.transparentPass);
        set(ShaderFeature::Skybox, options.skybox);
        set(ShaderFeature::BaseColorMap, options.baseColorMap);
        set(ShaderFeature::NormalMap, options.normalMap);
        set(ShaderFeature::MetallicRoughnessMap, options.metallicRoughnessMap);
        set(ShaderFeature::OcclusionMap, options.occlusionMap);
        set(ShaderFeature::EmissiveMap, options.emissiveMap);
        set(ShaderFeature::AlphaTest, options.alphaTest);
        set(ShaderFeature::DoubleSided, options.doubleSided);
        set(ShaderFeature::Shadows, options.shadowMapping);
        set(ShaderFeature::Fog, options.fog);
        set(ShaderFeature::VertexColors, options.vertexColors);
        set(ShaderFeature::PointSpotAttenuation, options.pointSpotAttenuation);
        set(ShaderFeature::MultiLight, options.multiLight);
        set(ShaderFeature::EnvAtlas, options.envAtlas);
        set(ShaderFeature::Parallax, options.parallax);
        set(ShaderFeature::Clearcoat, options.clearcoat);
        set(ShaderFeature::Anisotropy, options.anisotropy);
        set(ShaderFeature::Sheen, options.sheen);
        set(ShaderFeature::Iridescence, options.iridescence);
        set(ShaderFeature::Transmission, options.transmission);
        set(ShaderFeature::LightClustering, options.lightClustering);
        set(ShaderFeature::Ssao, options.ssao);
        set(ShaderFeature::LightProbes, options.lightProbes);
        set(ShaderFeature::Skinning, options.skinning);
        set(ShaderFeature::Morphing, options.morphing);
        set(ShaderFeature::SpecGloss, options.specGloss);
        set(ShaderFeature::OrenNayar, options.orenNayar);
        set(ShaderFeature::DetailNormals, options.detailNormals);
        set(ShaderFeature::Displacement, options.displacement);
        set(ShaderFeature::Atmosphere, options.atmosphere);
        set(ShaderFeature::ShadowCatcher, options.shadowCatcher);
        set(ShaderFeature::SkyCubemap, options.skyCubemap);
        set(ShaderFeature::Instancing, options.instancing);
        set(ShaderFeature::InstancingColor, options.instancingColor);
        set(ShaderFeature::PlanarReflection, options.planarReflection);
        set(ShaderFeature::PlanarReflectionDepthPass,
            options.planarReflectionDepthPass);
        set(ShaderFeature::LightmapBake, options.lightmapBake);
        set(ShaderFeature::LightmapBakeAccum, options.lightmapBakeAccum);
        set(ShaderFeature::DebugPass, options.debugPass);
        set(ShaderFeature::LocalShadows, options.localShadows);
        set(ShaderFeature::OmniShadows, options.omniShadows);
        set(ShaderFeature::Cookie2D, options.cookie2D);
        set(ShaderFeature::CookieCube, options.cookieCube);
        set(ShaderFeature::DynamicBatch, options.dynamicBatch);
        set(ShaderFeature::PointSize, options.pointSize);
        set(ShaderFeature::Unlit, options.unlit);
        set(ShaderFeature::AreaLights, options.areaLights);
        set(ShaderFeature::VsmShadows, options.vsmShadows);
        set(ShaderFeature::Lightmap, options.lightmap);
        set(ShaderFeature::DynamicRefraction, options.dynamicRefraction);
        set(ShaderFeature::OpacityDither, options.opacityDither);
        set(ShaderFeature::PcssShadows, options.pcssShadows);
        set(ShaderFeature::ReflectionProbe, options.reflectionProbe);
        set(ShaderFeature::Ssr, options.ssr);
        set(ShaderFeature::SurfaceLic, options.surfaceLIC);
        return features;
    }

    ProgramLibrary::VariantKey ProgramLibrary::makeVariantKey(const std::string& programName,
        const ShaderVariantOptions& options, const Material* material) const
    {
        // Build the key entirely from resolved ShaderVariantOptions — do NOT fold in
        // the raw material shaderVariantKey, because the options already capture
        // every flag that affects the compiled shader.  Including the raw key was
        // creating spurious unique variants (different materials mapping to the
        // same set of options but different shaderVariantKey values) and hitting
        // the AGX compiled-variants footprint limit.
        //
        // The key holds the feature set itself rather than a mask folded into an
        // integer, so the cache compares variants exactly: growing the feature list
        // past any word boundary cannot make two variants alias.
        VariantKey key;
        key.programNameHash = fnv1a64(programName);
        key.features = makeFeatureSet(options);

        // Shader chunk overrides (registry + per-material) change the composed
        // source without changing any feature — carry their content hashes so
        // overridden chunks compile fresh programs instead of hitting stale cache.
        key.chunksHash = _chunks.hash();
        if (material) {
            key.materialChunksHash = material->shaderChunksHash();
        }
        return key;
    }

    std::string ProgramLibrary::glslFeaturePreamble()
    {
        // Runtime twin of the shader_features.glsl that
        // tools/generate_vulkan_shader_bundle.py writes at build time. Both are
        // generated from the VT_SHADER_FEATURES list, so a runtime-composed module
        // and a bundled one read the same specialization constants and the pipeline
        // can specialize either identically.
        std::string source;
        source += "// Generated from platform/graphics/shaderFeatures.h at runtime.\n";
        for (size_t word = 0; word < kShaderFeatureWordCount; ++word) {
            source += "layout(constant_id = " + std::to_string(word) +
                ") const uint vtFeatureMask" + std::to_string(word) + " = 0u;\n";
        }
        source += "bool vtFeatureEnabled(uint bit) {\n";
        source += "    uint mask = 1u << (bit & 31u);\n";
        source += "    uint word = bit >> 5u;\n";
        for (size_t word = 0; word < kShaderFeatureWordCount; ++word) {
            source += "    if (word == " + std::to_string(word) +
                "u) return (vtFeatureMask" + std::to_string(word) + " & mask) != 0u;\n";
        }
        source += "    return false;\n}\n";
        uint32_t index = 0;
#define VT_APPEND_GLSL_FEATURE_BIT(symbol, defineName) \
        source += "const uint " defineName "_BIT = " + std::to_string(index++) + "u;\n";
        VT_SHADER_FEATURES(VT_APPEND_GLSL_FEATURE_BIT)
#undef VT_APPEND_GLSL_FEATURE_BIT
        return source;
    }

    std::string ProgramLibrary::composeProgramVariantGlslSource(const std::string& programName,
        const Material* material)
    {
        if (!_chunks.loaded()) {
            spdlog::error("Failed to load GLSL shader chunks from engine/shaders/vulkan/chunks.");
            return {};
        }
        const auto programChunks = _registeredPrograms.find(programName);
        if (programChunks == _registeredPrograms.end() || programChunks->second.empty()) {
            // "shadow" lands here: it has no chunked GLSL form. Report rather than
            // returning source that would silently replace the bundled module.
            spdlog::warn("ProgramLibrary: no GLSL chunk order for program '{}'; "
                "chunk overrides do not apply to it on Vulkan.", programName);
            return {};
        }

        std::string source;
        source.reserve(96 * 1024);
        source += "#version 450\n";
        source += glslFeaturePreamble();

        const auto* materialChunks = material ? &material->shaderChunkOverrides() : nullptr;
        for (const auto& chunkName : programChunks->second) {
            const std::string* chunkSource = nullptr;
            if (materialChunks) {
                if (const auto it = materialChunks->find(chunkName); it != materialChunks->end()) {
                    chunkSource = &it->second;
                }
            }
            if (!chunkSource) {
                chunkSource = _chunks.get(chunkName);
            }
            if (!chunkSource) {
                spdlog::error("ProgramLibrary GLSL chunk '{}' is missing in '{}'.",
                    chunkName, _chunks.rootPath().string());
                return {};
            }
            source += *chunkSource;
            source += "\n";
        }
        return source;
    }

    bool ProgramLibrary::hasChunkOverrides(const Material* material) const
    {
        if (_chunks.hash() != 0) {
            return true;
        }
        return material && material->shaderChunksHash() != 0;
    }

    void ProgramLibrary::warnUnsupportedGlslOverrides() const
    {
        // Vertex chunks have no Vulkan counterpart (prebuilt module family), so an
        // override of one would otherwise do nothing with no explanation. Warn once
        // per name — the whole point of this path is that overrides stop being silent.
        static const std::array<const char*, 2> vertexOnly = {"forward-vertex", "shadow-vertex"};
        for (const char* name : vertexOnly) {
            if (_chunks.overrides().count(name) == 0) {
                continue;
            }
            if (_warnedFeatureFlags.insert(std::string("glsl-vertex-chunk:") + name).second) {
                spdlog::warn("ProgramLibrary: chunk '{}' was overridden, but the Vulkan "
                    "backend builds its vertex stage from prebuilt modules — the override "
                    "applies on Metal only.", name);
            }
        }
    }

    std::string ProgramLibrary::composeProgramVariantMetalSource(const std::string& programName, const ShaderVariantOptions& options,
        const std::string& vertexEntry, const std::string& fragmentEntry, const Material* material)
    {
        if (!_chunks.loaded()) {
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

        // The feature names/bits consumed by Metal and Vulkan are generated
        // from one contract. Metal receives defines; Vulkan receives this same
        // mask through SPIR-V specialization constants.
        const ShaderFeatureSet features = makeFeatureSet(options);
#define VT_APPEND_METAL_FEATURE(symbol, defineName) \
        appendFeatureDefine(source, defineName, features.test(ShaderFeature::symbol));
        VT_SHADER_FEATURES(VT_APPEND_METAL_FEATURE)
#undef VT_APPEND_METAL_FEATURE
        // VT_FEATURE_HDR_PASS is not emitted as a compile-time define.
        // It is passed as a runtime uniform bit in LightingData.flagsAndPad
        // to avoid doubling the number of compiled shader variants.

        source += "\n#define VT_VERTEX_ENTRY ";
        source += vertexEntry;
        source += "\n#define VT_FRAGMENT_ENTRY ";
        source += fragmentEntry;
        source += "\n\n";

        // Chunk resolution order mirrors upstream: per-material override, then the
        // device registry override, then the default source.
        const auto* materialChunks = material ? &material->shaderChunkOverrides() : nullptr;
        for (const auto& chunkName : programChunks->second) {
            const std::string* chunkSource = nullptr;
            if (materialChunks) {
                if (const auto it = materialChunks->find(chunkName); it != materialChunks->end()) {
                    chunkSource = &it->second;
                }
            }
            if (!chunkSource) {
                chunkSource = _chunks.get(chunkName);
            }
            if (!chunkSource) {
                spdlog::error("ProgramLibrary chunk '{}' is missing in '{}'.",
                    chunkName, _chunks.rootPath().string());
                return {};
            }
            source += *chunkSource;
            source += "\n";
        }

        return source;
    }

    std::shared_ptr<Shader> ProgramLibrary::buildForwardShaderVariant(const std::string& programName,
        const ShaderVariantOptions& options, const uint64_t variantId, const Material* material)
    {
        // variantId only names the generated entry points. Each variant compiles as
        // its own translation unit, so the name just has to be internally consistent —
        // variant identity itself is the exact VariantKey the cache compares.
        ShaderDefinition definition;
        definition.name = "program-" + programName;
        definition.name += options.transparentPass ? "-transparent" : "-opaque";
        definition.name += "-" + std::to_string(variantId);
        const auto entryPrefix = programName == "shadow" ? "pcShadow" : "pcForward";
        definition.vshader = entryPrefix + std::string("VS_") + std::to_string(variantId);
        definition.fshader = entryPrefix + std::string("FS_") + std::to_string(variantId);
        definition.features = makeFeatureSet(options);

        if (_chunks.language() == ShaderLanguage::Glsl) {
            // Vulkan's default path is the build-time SPIR-V bundle, which already IS
            // these chunks compiled — composing and recompiling identical source every
            // time would cost startup for nothing. Source is handed over only when an
            // override actually changes it; an empty string selects the bundle.
            if (!hasChunkOverrides(material)) {
                return createShader(_device.get(), definition, {});
            }
            warnUnsupportedGlslOverrides();
            const std::string glsl = composeProgramVariantGlslSource(programName, material);
            if (glsl.empty()) {
                // No chunked GLSL form for this program (e.g. shadow) — fall back to
                // the bundled module rather than failing the draw. Already warned.
                return createShader(_device.get(), definition, {});
            }
            return createShader(_device.get(), definition, glsl);
        }

        const std::string sourceCode = composeProgramVariantMetalSource(programName, options, definition.vshader, definition.fshader, material);
        if (sourceCode.empty()) {
            return nullptr;
        }
        return createShader(_device.get(), definition, sourceCode);
    }

    std::shared_ptr<Shader> ProgramLibrary::getForwardShader(const Material* material, const bool transparentPass,
        const bool dynamicBatch, const bool skinning, const bool morphing,
        const bool instancing, const bool instancingColor)
    {
        if (!_device) {
            return nullptr;
        }

        const ShaderVariantOptions options = buildForwardVariantOptions(material, transparentPass, dynamicBatch,
            skinning, morphing, instancing, instancingColor);
        const std::string programName = resolveProgramName(options);
        if (!hasProgram(programName)) {
            spdlog::error("ProgramLibrary has no registered program '{}'.", programName);
            return nullptr;
        }
        // Registry override change: purge cached variants so recompiled programs
        // do not pile up next to stale ones (AGX compiled-variants footprint).
        if (_chunks.hash() != _cachedChunksHash) {
            _forwardShaderCache.clear();
            _cachedChunksHash = _chunks.hash();
        }

        const VariantKey key = makeVariantKey(programName, options, material);

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
        // lightProbes: fully implemented — no warning needed.
        // vertexColors: fully implemented — no warning needed.
        // skinning: fully implemented — no warning needed.
        // morphing: fully implemented — no warning needed.
        // specGloss/orenNayar/detailNormals/displacement: fully implemented.
        // atmosphere: fully implemented — no warning needed.

        const uint64_t variantId = key.hash();
        auto shader = buildForwardShaderVariant(programName, options, variantId, material);
        if (!shader) {
            spdlog::error("Failed to build shader variant '{}' (id={:#x}, localShadows={}, shadows={}, envAtlas={})",
                programName, variantId, options.localShadows, options.shadowMapping, options.envAtlas);
        }
        _forwardShaderCache[key] = shader;
        return shader;
    }

    std::shared_ptr<Shader> ProgramLibrary::getShadowShader(const bool dynamicBatch, const bool skinning,
        const bool morphing, const bool instancing, const bool instancingColor)
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
        options.skinning = skinning;
        options.morphing = morphing;
        // Instanced casters transform through the per-instance matrix in the shadow
        // vertex stage, exactly as they do in the forward pass — without this the
        // whole cloud would collapse onto the mesh instance's own node transform.
        options.instancing = instancing;
        options.instancingColor = instancing && instancingColor;
        // The shadow fragment shader needs to know whether to write moments
        // (RGBA16F EVSM) or just rely on hardware depth (PCF). Both shadow
        // shader variants are cached separately by the variant key.
        options.vsmShadows = _vsmShadowsEnabled;

        const VariantKey key = makeVariantKey("shadow", options, nullptr);
        const auto cached = _forwardShaderCache.find(key);
        if (cached != _forwardShaderCache.end()) {
            return cached->second;
        }

        auto shader = buildForwardShaderVariant("shadow", options, key.hash());
        _forwardShaderCache[key] = shader;
        return shader;
    }

    void ProgramLibrary::bindMaterial(const std::shared_ptr<GraphicsDevice>& device, const Material* material,
        const bool transparentPass, const bool dynamicBatch, const bool skinning, const bool morphing,
        const bool instancing, const bool instancingColor)
    {
        if (!device) {
            return;
        }

        auto shader = material ? material->shaderOverride() : nullptr;
        if (!shader) {
            shader = getForwardShader(material, transparentPass, dynamicBatch, skinning, morphing,
                instancing, instancingColor);
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
            // Polygon offset (decals, coplanar overlays). Reset to zero on every
            // material that does not request bias — otherwise a prior decal draw's
            // offset would leak onto the next opaque draw on the same encoder.
            device->setDepthBias(depthState->depthBias(), depthState->slopeDepthBias(), 0.0f);
        }
        device->setMaterial(material);
    }
}
