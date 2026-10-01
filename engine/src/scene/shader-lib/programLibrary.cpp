// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#include "programLibrary.h"

#include <cstring>

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
            "common-sheen",
            "common-iridescence",
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

    namespace
    {
        bool variantBit(const uint64_t variantBits, const int bit)
        {
            return (variantBits & (1ull << bit)) != 0ull;
        }
    }

    // The options come from four sources, applied in this order: the material (typed
    // StandardMaterial properties, or a generic material's parameters and variant-key
    // bits), the variant-key bits both kinds honour, the draw, and the frame-wide
    // switches the renderer sets before the draw loop.
    ProgramLibrary::ShaderVariantOptions ProgramLibrary::buildForwardVariantOptions(const Material* material,
        const bool transparentPass, const bool dynamicBatch, const bool skinning, const bool morphing,
        const bool instancing, const bool instancingColor, const bool instanceLightmap, const bool screenSpace) const
    {
        ShaderVariantOptions options{};
        options.transparentPass = transparentPass;
        options.screenSpace = screenSpace;
        options.skybox = material && material->isSkybox();
        options.alphaTest = material && material->alphaMode() == AlphaMode::MASK;

        const uint64_t variantBits = material ? material->shaderVariantKey() : 0ull;
        if (const auto* stdMat = dynamic_cast<const StandardMaterial*>(material)) {
            applyStandardMaterialOptions(options, *stdMat);
        } else {
            applyGenericMaterialOptions(options, material, variantBits);
        }
        applyVariantKeyOptions(options, variantBits);
        applyDrawOptions(options, variantBits, dynamicBatch, skinning, morphing, instancing, instancingColor,
            instanceLightmap);
        applyFrameOptions(options, variantBits);
        return options;
    }

    void ProgramLibrary::applyStandardMaterialOptions(ShaderVariantOptions& options,
        const StandardMaterial& stdMat) const
    {
        // StandardMaterial stores twoSidedLighting as a separate flag.
        options.doubleSided = stdMat.cullMode() == CullMode::CULLFACE_NONE || stdMat.twoSidedLighting();

        // Prefer StandardMaterial-specific textures, fall back to base Material typed properties.
        options.baseColorMap = (stdMat.diffuseMap() || stdMat.baseColorTexture());
        options.normalMap = (stdMat.normalMap() || stdMat.normalTexture());
        options.metallicRoughnessMap = (stdMat.metalnessMap() || stdMat.metallicRoughnessTexture());
        options.occlusionMap = (stdMat.aoMap() || stdMat.occlusionTexture());
        options.emissiveMap = (stdMat.emissiveMap() || stdMat.emissiveTexture());

        options.fog = stdMat.useFog() && !options.skybox;
        options.parallax = stdMat.heightMap() != nullptr;
        options.clearcoat = stdMat.clearCoat() > 0.0f;
        options.anisotropy = stdMat.anisotropy() != 0.0f;
        options.sheen = stdMat.sheenRoughness() > 0.0f || stdMat.sheenColor() != Color(0.0f, 0.0f, 0.0f, 1.0f);
        options.iridescence = stdMat.iridescenceIntensity() > 0.0f;
        options.transmission = stdMat.transmissionFactor() > 0.0f;
        // Dynamic grab-pass refraction: an opt-in that requires transmission.
        options.dynamicRefraction = options.transmission && stdMat.useDynamicRefraction();
        options.ssr = stdMat.useScreenSpaceReflection();
        // Opacity dither: Bayer8 dithered transparency in the opaque pass.
        options.opacityDither = stdMat.opacityDither();
        options.lightmap = stdMat.lightMap() != nullptr;
        // Vertex colors are otherwise an explicit opt-in (variant bit 21) because the
        // material cannot see whether the mesh even carries a color stream — but asking
        // for emissiveVertexColor is that opt-in, and would otherwise silently do nothing.
        options.vertexColors = stdMat.emissiveVertexColor();
        // The specular workflow (useMetalness false, upstream's default) runs through
        // the spec-gloss variant: F0 from the specular colour, gloss from `gloss`.
        options.specGloss = stdMat.usesSpecularWorkflow() || stdMat.specGlossMap() != nullptr;
        // Upstream's useSpecular: a default StandardMaterial renders no specular at all.
        options.noSpecular = !stdMat.rendersSpecular();
        options.orenNayar = stdMat.useOrenNayar();
        options.detailNormals = stdMat.detailNormalMap() != nullptr;
        options.displacement = stdMat.displacementMap() != nullptr;
        // A StandardMaterial with lighting disabled (decals, debug visualizers, holograms).
        options.unlit = !stdMat.useLighting();
        options.msdf = stdMat.msdfMap() != nullptr;
        options.shadowCatcher = stdMat.shadowCatcher();
        // DEVIATION: planar reflection is handled at the application level as a script;
        // here it's a material property that triggers a shader variant.
        options.planarReflection = stdMat.reflectionMap() != nullptr;

        // The opacity map has a Metal slot (34) and none on Vulkan, whose fragment stage
        // is already at MoltenVK's 16-sampler limit.
        if (stdMat.opacityMap() && _chunks.language() == ShaderLanguage::Glsl &&
            _warnedFeatureFlags.insert("opacityMap").second) {
            spdlog::warn("StandardMaterial::opacityMap is not supported on the Vulkan backend "
                "(material '{}'); opacity comes from the base colour alpha only", stdMat.name());
        }
    }

    void ProgramLibrary::applyGenericMaterialOptions(ShaderVariantOptions& options, const Material* material,
        const uint64_t variantBits)
    {
        CullMode effectiveCullMode = material ? material->cullMode() : CullMode::CULLFACE_BACK;
        if (int cullModeValue = static_cast<int>(effectiveCullMode);
            readParameterInt(getMaterialParameter(material, {"material_cullMode", "cullMode"}), cullModeValue)) {
            if (cullModeValue >= static_cast<int>(CullMode::CULLFACE_NONE) &&
                cullModeValue <= static_cast<int>(CullMode::CULLFACE_FRONTANDBACK)) {
                effectiveCullMode = static_cast<CullMode>(cullModeValue);
            }
        }
        options.doubleSided = effectiveCullMode == CullMode::CULLFACE_NONE;

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

        bool skyboxOverride = options.skybox;
        if (readParameterBool(getMaterialParameter(material, {"material_isSkybox", "isSkybox"}), skyboxOverride)) {
            options.skybox = skyboxOverride;
        }

        // Every feature a StandardMaterial reads from a typed property, a generic
        // material takes from its shaderVariantKey.
        options.fog = !options.skybox || variantBit(variantBits, 11);
        options.parallax = variantBit(variantBits, 12);
        options.clearcoat = variantBit(variantBits, 13);
        options.anisotropy = variantBit(variantBits, 14);
        options.sheen = variantBit(variantBits, 15);
        options.iridescence = variantBit(variantBits, 16);
        options.transmission = variantBit(variantBits, 17);
        options.specGloss = variantBit(variantBits, 24);
        options.orenNayar = variantBit(variantBits, 25);
        options.detailNormals = variantBit(variantBits, 26);
        options.displacement = variantBit(variantBits, 27);
        options.lightmap = variantBit(variantBits, 34);
    }

    void ProgramLibrary::applyVariantKeyOptions(ShaderVariantOptions& options, const uint64_t variantBits)
    {
        // Bits every material honours, StandardMaterial included.
        options.shadowMapping = !options.skybox || variantBit(variantBits, 10);
        options.vertexColors = options.vertexColors || variantBit(variantBits, 21);
        options.pointSpotAttenuation = !options.skybox || variantBit(variantBits, 29);
        options.multiLight = !options.skybox || variantBit(variantBits, 30);
        options.pointSize = variantBit(variantBits, 31);
        // unlit: bit 32, used by glb-parser for KHR_materials_unlit assets.
        options.unlit = options.unlit || variantBit(variantBits, 32);
    }

    void ProgramLibrary::applyDrawOptions(ShaderVariantOptions& options, const uint64_t variantBits,
        const bool dynamicBatch, const bool skinning, const bool morphing, const bool instancing,
        const bool instancingColor, const bool instanceLightmap)
    {
        // A mesh instance's own lightmap needs the path whatever the material says;
        // the device binds it over the material's (upstream useInstanceLightMap).
        options.lightmap = options.lightmap || instanceLightmap;
        // Skinning/morphing are per-draw flags set by the renderer from
        // MeshInstance::skinInstance()/morphInstance(); the variant-key bits
        // remain as a material-level override.
        options.skinning = skinning || variantBit(variantBits, 22);
        options.morphing = morphing || variantBit(variantBits, 23);
        // Instancing follows the draw: a mesh instance with a per-instance buffer gets the
        // instanced vertex stage (upstream infers it from MeshInstance::setInstancing the same
        // way). Variant bit 33 is honoured so materials that opt in explicitly keep working;
        // for those the per-instance color is assumed, since such materials expect the
        // 80-byte layout, which carries one.
        const bool materialInstancing = variantBit(variantBits, 33);
        options.instancing = instancing || materialInstancing;
        options.instancingColor = instancing ? instancingColor : materialInstancing;
        // Dynamic batching: per-vertex bone index + matrix palette.
        // Set by the renderer from MeshInstance::isDynamicBatch().
        options.dynamicBatch = dynamicBatch;
    }

    void ProgramLibrary::applyFrameOptions(ShaderVariantOptions& options, const uint64_t variantBits) const
    {
        // Set by the renderer for the frame (or the camera) before the draw loop.
        options.envAtlas = _envAtlasEnabled;
        options.reflectionProbe = _reflectionProbeEnabled;
        options.lightClustering = _clusteredLightingEnabled || variantBit(variantBits, 18);
        options.ssao = _ssaoEnabled || variantBit(variantBits, 19);
        options.lightProbes = _lightProbesEnabled || variantBit(variantBits, 20);
        options.atmosphere = (_atmosphereEnabled && options.skybox) || variantBit(variantBits, 28);

        // When a skybox cubemap is available, the skybox shader samples it instead
        // of the envAtlas.
        if (options.skybox && _skyCubemapAvailable) {
            options.skyCubemap = true;
        }

        // Depth pass: the fragment shader outputs distance-from-plane instead of PBR.
        options.planarReflectionDepthPass = _planarReflectionDepthPass;
        options.lightmapBake = _lightmapBakePass;
        options.lightmapBakeAccum = _lightmapBakeAccumulate;
        // A bake computes the light afresh. With a previous bake still attached (to
        // the mesh instance or the material) the lightmap path would replace the
        // ambient with it, and the non-accumulating pass writes that indirect term
        // back out — so every re-bake fed on the one before.
        if (options.lightmapBake) {
            options.lightmap = false;
        }

        // Debug surface-quantity output; the camera's debugShaderPass picks the mode.
        options.debugPass = _debugPassEnabled;

        // Local (spot) and omni shadows, cookies, directional EVSM and area lights:
        // enabled when some light in the scene needs them. The skybox never does.
        options.localShadows = _localShadowsEnabled && !options.skybox;
        options.omniShadows = _omniShadowsEnabled && !options.skybox;
        options.cookie2D = _cookie2DEnabled && !options.skybox;
        options.cookieCube = _cookieCubeEnabled && !options.skybox;
        options.vsmShadows = _vsmShadowsEnabled && !options.skybox;
        options.pcssShadows = _pcssShadowsEnabled;
        options.areaLights = _areaLightsEnabled && !options.skybox;
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
        set(ShaderFeature::NoSpecular, options.noSpecular);
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
        set(ShaderFeature::ScreenSpace, options.screenSpace);
        set(ShaderFeature::Msdf, options.msdf);
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
        set(ShaderFeature::ShadowDither, options.shadowDither);
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
        // every flag that affects the compiled shader.  Including the raw key would
        // create spurious unique variants (different materials mapping to the
        // same set of options but different shaderVariantKey values) and hit
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

    void ProgramLibrary::substituteMaterialBlock(std::string& source, const bool msl)
    {
        constexpr const char* marker = "VT_MATERIAL_DATA_BLOCK";
        const auto at = source.find(marker);
        if (at == std::string::npos) {
            return;
        }
        std::string block = "struct MaterialData {\n";
        block += materialUniformDeclaration(msl);
        block += "};";
        source.replace(at, std::strlen(marker), block);
    }

    std::string ProgramLibrary::glslMaterialBlock()
    {
        // Runtime twin of the shader_material.glsl the bundle generator writes.
        std::string s = "layout(set = 0, binding = 0) uniform MaterialData {\n";
        s += materialUniformDeclaration(/*msl=*/false);
        s += "} material;\n";
        return s;
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
        source += glslMaterialBlock();

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

        // The material block is emitted from materialUniformFields.h, so the C++
        // struct and the shader declaration cannot drift. The chunk carries a
        // VT_MATERIAL_DATA_BLOCK marker where the struct is substituted.
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

        substituteMaterialBlock(source, /*msl=*/true);
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
        const bool instancing, const bool instancingColor, const bool instanceLightmap, const bool screenSpace)
    {
        if (!_device) {
            return nullptr;
        }

        const ShaderVariantOptions options = buildForwardVariantOptions(material, transparentPass, dynamicBatch,
            skinning, morphing, instancing, instancingColor, instanceLightmap, screenSpace);
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

        const uint64_t variantId = key.hash();
        auto shader = buildForwardShaderVariant(programName, options, variantId, material);
        if (!shader) {
            spdlog::error("Failed to build shader variant '{}' (id={:#x}, localShadows={}, shadows={}, envAtlas={})",
                programName, variantId, options.localShadows, options.shadowMapping, options.envAtlas);
        }
        _forwardShaderCache[key] = shader;
        return shader;
    }

    bool ProgramLibrary::shadowNeedsMaterial(const Material* material)
    {
        if (!material) {
            return false;
        }
        if (material->alphaMode() == AlphaMode::MASK) {
            return true;
        }
        const auto* stdMat = dynamic_cast<const StandardMaterial*>(material);
        return stdMat && stdMat->opacityShadowDitherMode() != DitherMode::DITHER_NONE;
    }

    std::shared_ptr<Shader> ProgramLibrary::getShadowShader(const Material* material,
        const bool dynamicBatch, const bool skinning,
        const bool morphing, const bool instancing, const bool instancingColor, const bool vsm)
    {
        if (!_device) {
            return nullptr;
        }
        // The GLSL backend builds its shadow shader from prebuilt bundle modules
        // selected by the definition NAME, not by chunk composition, so
        // registerGlslPrograms deliberately registers no "shadow" chunk program.
        // Requiring one here would silently disable EVERY Vulkan shadow: the shadow
        // passes return as soon as this is null, so nothing would be drawn into the
        // shadow map, it would stay at its cleared 1.0, and every fragment would read
        // as lit.
        // buildForwardShaderVariant already falls back to the bundle for a program
        // with no chunked GLSL form, so the check is only meaningful for MSL.
        if (_chunks.language() != ShaderLanguage::Glsl && !hasProgram("shadow")) {
            return nullptr;
        }

        const auto* stdMat = dynamic_cast<const StandardMaterial*>(material);

        ShaderVariantOptions options{};
        options.skybox = false;
        options.transparentPass = false;
        // The shadow pass runs the material's opacity frontend before writing depth,
        // as upstream's litShadowMain does. Without it a masked material — foliage,
        // a chain-link fence, a cut-out sign — writes depth over its whole quad and
        // throws a solid shadow.
        options.alphaTest = material && material->alphaMode() == AlphaMode::MASK;
        options.baseColorMap = options.alphaTest &&
            (stdMat ? (stdMat->diffuseMap() != nullptr || stdMat->baseColorTexture() != nullptr)
                    : (material && material->hasBaseColorTexture()));
        options.shadowDither = stdMat &&
            stdMat->opacityShadowDitherMode() != DitherMode::DITHER_NONE;
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
        // (RGBA16F EVSM) or just rely on hardware depth (PCF). That is a property
        // of the LIGHT being rendered, so the caller says it. Do not take it from the
        // scene-wide VSM switch: renderForwardLayer sets that AFTER the frame's shadow
        // passes have run, so a light's first VSM shadow would use the depth-only
        // variant and write no moments (a one-shot shadow would stay blank for good),
        // and spot shadows and the depth prepass would get the moments variant under a
        // VSM key light.
        options.vsmShadows = vsm;

        const VariantKey key = makeVariantKey("shadow", options, material);
        const auto cached = _forwardShaderCache.find(key);
        if (cached != _forwardShaderCache.end()) {
            return cached->second;
        }

        auto shader = buildForwardShaderVariant("shadow", options, key.hash(), material);
        _forwardShaderCache[key] = shader;
        return shader;
    }

    void ProgramLibrary::bindMaterial(const std::shared_ptr<GraphicsDevice>& device, const Material* material,
        const bool transparentPass, const bool dynamicBatch, const bool skinning, const bool morphing,
        const bool instancing, const bool instancingColor, const bool instanceLightmap, const bool screenSpace)
    {
        if (!device) {
            return;
        }

        auto shader = material ? material->shaderOverride() : nullptr;
        if (!shader) {
            shader = getForwardShader(material, transparentPass, dynamicBatch, skinning, morphing,
                instancing, instancingColor, instanceLightmap, screenSpace);
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
