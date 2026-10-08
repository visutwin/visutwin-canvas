// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025
//
#include "programLibrary.h"

#include "shaderChunks.h"

#include <assert.h>
#include <algorithm>
#include <cstdint>
#include <vector>

#include "core/hash.h"
#include "platform/graphics/slangCompiler.h"
#include "platform/graphics/deviceCache.h"
#include "scene/materials/materialParameterRead.h"
#include "scene/renderer/cullModeResolve.h"
#include "spdlog/spdlog.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    namespace
    {
        DeviceCache programLibraryDeviceCache;
        std::unordered_map<GraphicsDevice*, std::shared_ptr<ProgramLibrary>> programLibraries;
        
        bool hasTextureParameter(const Material* material, std::initializer_list<const char*> names)
        {
            if (const auto* value = findMaterialParameter(material, names)) {
                if (const auto* texture = std::get_if<Texture*>(value)) {
                    return *texture != nullptr;
                }
            }
            return false;
        }
    }

    namespace
    {
        uint64_t nextProgramLibrarySerial()
        {
            static uint64_t serial = 0;
            return ++serial;
        }
    }

    ProgramLibrary::ProgramLibrary(const std::shared_ptr<GraphicsDevice>& device)
        : _device(device),
          _serial(nextProgramLibrarySerial())
    {
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
        options.pick = material && material->pickPass();

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
        options.vertexColorGamma = stdMat.vertexColorGamma();
        // The specular workflow (useMetalness false, the default) runs through
        // the spec-gloss variant: F0 from the specular colour, gloss from `gloss`.
        options.specGloss = stdMat.usesSpecularWorkflow() || stdMat.specGlossMap() != nullptr;
        // A default StandardMaterial renders no specular at all.
        options.noSpecular = !stdMat.rendersSpecular();
        options.orenNayar = stdMat.useOrenNayar();
        options.detailNormals = stdMat.detailNormalMap() != nullptr;
        options.displacement = stdMat.displacementMap() != nullptr;
        // useLighting off keeps the lit pipeline (ambient, reflections, fog, the
        // combine) and drops only the lights; applyFrameOptions turns the clustered
        // lights off for it.
        options.noLights = !stdMat.useLighting();
        options.msdf = stdMat.msdfMap() != nullptr;
        // The fully unlit output (UI, MSDF text, overlays). An MSDF map is read as a
        // distance field only on that path, so it implies it.
        options.unlit = stdMat.unlit() || options.msdf;
        // useTonemap off (UI text and images): neither the curve nor exposure, while
        // the gamma encode of a gamma target still applies.
        options.noTonemap = !stdMat.useTonemap();
        options.shadowCatcher = stdMat.shadowCatcher();
        // DEVIATION: planar reflection is handled at the application level as a script;
        // here it's a material property that triggers a shader variant.
        options.planarReflection = stdMat.reflectionMap() != nullptr;

    }

    void ProgramLibrary::applyGenericMaterialOptions(ShaderVariantOptions& options, const Material* material,
        const uint64_t variantBits)
    {
        options.doubleSided = resolveMaterialCullMode(material) == CullMode::CULLFACE_NONE;

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
        if (readParameterBool(findMaterialParameter(material, {"material_isSkybox", "isSkybox"}), skyboxOverride)) {
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
        // Gamma-encoded vertex colours: bit 35, for a material that is not a
        // StandardMaterial (which says StandardMaterial::setVertexColorGamma).
        options.vertexColorGamma = options.vertexColorGamma || variantBit(variantBits, 35);
    }

    void ProgramLibrary::applyDrawOptions(ShaderVariantOptions& options, const uint64_t variantBits,
        const bool dynamicBatch, const bool skinning, const bool morphing, const bool instancing,
        const bool instancingColor, const bool instanceLightmap)
    {
        // A mesh instance's own lightmap needs the path whatever the material says;
        // the device binds it over the material's.
        options.lightmap = options.lightmap || instanceLightmap;
        // Skinning/morphing are per-draw flags set by the renderer from
        // MeshInstance::skinInstance()/morphInstance(); the variant-key bits
        // remain as a material-level override.
        options.skinning = skinning || variantBit(variantBits, 22);
        options.morphing = morphing || variantBit(variantBits, 23);
        // Instancing follows the draw: a mesh instance with a per-instance buffer gets the
        // instanced vertex stage. Variant bit 33 is honoured so materials that opt in explicitly keep working;
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
        options.pcf1Shadows = _pcf1ShadowsEnabled && !options.skybox;
        options.pcf5Shadows = _pcf5ShadowsEnabled && !options.skybox;
        options.areaLights = _areaLightsEnabled && !options.skybox;

        // A material that takes no lights (useLighting off) gets no clustered lights
        // either, and nothing that only shades a light — the light loops are empty.
        if (options.noLights) {
            options.lightClustering = false;
            options.areaLights = false;
            options.localShadows = false;
            options.omniShadows = false;
            options.cookie2D = false;
            options.cookieCube = false;
        }
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
        set(ShaderFeature::NoTonemap, options.noTonemap);
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
        set(ShaderFeature::NoLights, options.noLights);
        set(ShaderFeature::VertexColorGamma, options.vertexColorGamma);
        set(ShaderFeature::Pick, options.pick);
        set(ShaderFeature::AreaLights, options.areaLights);
        set(ShaderFeature::VsmShadows, options.vsmShadows);
        set(ShaderFeature::Lightmap, options.lightmap);
        set(ShaderFeature::DynamicRefraction, options.dynamicRefraction);
        set(ShaderFeature::OpacityDither, options.opacityDither);
        set(ShaderFeature::ShadowDither, options.shadowDither);
        set(ShaderFeature::PcssShadows, options.pcssShadows);
        set(ShaderFeature::Pcf1Shadows, options.pcf1Shadows);
        set(ShaderFeature::Pcf5Shadows, options.pcf5Shadows);
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

    namespace
    {
        const std::vector<uint32_t>* entrySpirv(const SlangProgramBuild& build, const std::string_view name)
        {
            for (const auto& entry : build.entries) {
                if (entry.name == name) {
                    return &entry.spirv;
                }
            }
            return nullptr;
        }
    }

    const SlangProgramBuild* ProgramLibrary::slangProgram(const std::string& program, const SlangFileOverrides& overrides)
    {
        uint64_t fingerprint = fnv1a64(program);
        for (const auto& [name, source] : overrides) {
            fingerprint = fnv1a64(source, fnv1a64(name, fingerprint));
        }
        const std::string key = program + ":" + std::to_string(fingerprint);
        if (const auto found = _slangPrograms.find(key); found != _slangPrograms.end()) {
            return found->second.get();
        }
        auto build = std::make_shared<SlangProgramBuild>();
        if (!loadSlangProgram(_device.get(), program, {}, overrides, *build)) {
            build.reset();
        }
        _slangPrograms[key] = build;
        return build.get();
    }

    /// The forward vertex entry for a variant: the vertex layout and deformation the draw
    /// has, in the priority Vulkan's pipeline applies to its family of modules.
    const char* ProgramLibrary::forwardVertexEntry(const ShaderVariantOptions& options)
    {
        if (options.dynamicBatch) {
            return "forwardDynamicBatchVertex";
        }
        if (options.skinning) {
            return options.morphing ? "forwardSkinnedMorphedVertex" : "forwardSkinnedVertex";
        }
        if (options.morphing) {
            return "forwardMorphedVertex";
        }
        if (options.skybox) {
            return "forwardSkyVertex";
        }
        if (options.instancing) {
            return options.instancingColor ? "forwardInstancedColorVertex" : "forwardInstancedVertex";
        }
        if (options.pointSize && options.vertexColors) {
            return "forwardPointVertex";
        }
        if (options.vertexColors) {
            return "forwardColorVertex";
        }
        return "forwardVertex";
    }

    SlangFileOverrides ProgramLibrary::chunkOverrides(const Material* material) const
    {
        // A chunk override is a file of the program's tree, by chunk name: the registry's
        // first, the material's on top. Sorted, so the fingerprint does not depend on the
        // maps' order.
        std::unordered_map<std::string, std::string> merged = _chunks.overrides();
        if (material) {
            for (const auto& [name, source] : material->shaderChunkOverrides()) {
                merged[name] = source;
            }
        }
        SlangFileOverrides overrides;
        overrides.reserve(merged.size());
        for (const auto& [name, source] : merged) {
            overrides.emplace_back(name + ".slang", source);
        }
        std::sort(overrides.begin(), overrides.end());
        return overrides;
    }

    std::shared_ptr<Shader> ProgramLibrary::buildForwardShaderVariant(const std::string& programName,
        const ShaderVariantOptions& options, const uint64_t variantId, const Material* material)
    {
        // "skybox" is the forward program under another name (VT_FEATURE_SKYBOX).
        const bool shadow = programName == "shadow";
        const SlangProgramBuild* build = slangProgram(shadow ? "shadow" : "forward", chunkOverrides(material));
        if (!build) {
            return nullptr;
        }

        ShaderDefinition definition;
        definition.name = "program-" + programName;
        definition.name += options.transparentPass ? "-transparent" : "-opaque";
        definition.name += "-" + std::to_string(variantId);
        definition.features = makeFeatureSet(options);
        definition.vshader = forwardVertexEntry(options);
        // The shadow pass's fragment stage: the moments writer for a VSM light, the opacity
        // frontend otherwise. Vulkan runs no fragment stage at all for a caster that needs no
        // frontend; Metal keeps the frontend there, which reads only its material's flags.
        const bool metal = _device->shaderLanguage() == ShaderLanguage::Msl;
        const bool needsOpacity = options.alphaTest || options.shadowDither;
        if (shadow) {
            definition.fshader = options.vsmShadows ? "shadowVsmFragment"
                : (needsOpacity || metal) ? "shadowOpacityFragment" : "";
        } else {
            definition.fshader = "forwardFragment";
        }

        ShaderCode code;
        // Both programs are specialized by the variant's features, the shadow one included:
        // its vertex stage is the forward one (displacement, skinning, instancing), and Metal
        // refuses a function that reads function constants unspecialized. The shadow
        // fragment stages gate on material flags, not features.
        code.specializeFeatures = true;
        code.depthOnlyFragment = shadow && !options.vsmShadows;
        if (metal) {
            code.metalLibrary = build->metalLibrary;
            code.metalSource = build->metalSource;
        } else {
            const auto* vertex = entrySpirv(*build, "forwardVertex");
            if (!vertex) {
                spdlog::error("ProgramLibrary: the Slang program '{}' has no forwardVertex entry", programName);
                return nullptr;
            }
            code.vertexSpirv = *vertex;
            if (!definition.fshader.empty()) {
                const auto* fragment = entrySpirv(*build, definition.fshader);
                if (!fragment) {
                    spdlog::error("ProgramLibrary: the Slang program '{}' has no {} entry", programName,
                        definition.fshader);
                    return nullptr;
                }
                code.fragmentSpirv = *fragment;
            }
            for (const auto& entry : build->entries) {
                if (entry.stage == "vertex" && entry.name != "forwardVertex") {
                    code.vertexFamily.push_back({entry.name, entry.spirv});
                }
            }
        }
        return _device->createShaderFromCode(definition, code);
    }

    namespace
    {
        // The draw's flags take the low bits of a memo's state word, the frame
        // switches the rest.
        constexpr unsigned kForwardDrawBitCount = 8;
    }

    uint64_t ProgramLibrary::forwardFrameBits() const
    {
        uint64_t bits = 0;
        unsigned index = 0;
        const auto add = [&bits, &index](const bool value) {
            bits |= static_cast<uint64_t>(value) << index++;
        };
        // Every switch applyFrameOptions reads.
        add(_envAtlasEnabled);
        add(_reflectionProbeEnabled);
        add(_clusteredLightingEnabled);
        add(_ssaoEnabled);
        add(_lightProbesEnabled);
        add(_atmosphereEnabled);
        add(_skyCubemapAvailable);
        add(_planarReflectionDepthPass);
        add(_lightmapBakePass);
        add(_lightmapBakeAccumulate);
        add(_debugPassEnabled);
        add(_localShadowsEnabled);
        add(_omniShadowsEnabled);
        add(_cookie2DEnabled);
        add(_cookieCubeEnabled);
        add(_vsmShadowsEnabled);
        add(_pcssShadowsEnabled);
        add(_pcf1ShadowsEnabled);
        add(_pcf5ShadowsEnabled);
        add(_areaLightsEnabled);
        return bits;
    }

    uint64_t ProgramLibrary::forwardStateBits(const bool transparentPass, const bool dynamicBatch,
        const bool skinning, const bool morphing, const bool instancing, const bool instancingColor,
        const bool instanceLightmap, const bool screenSpace) const
    {
        uint64_t bits = 0;
        unsigned index = 0;
        const auto add = [&bits, &index](const bool value) {
            bits |= static_cast<uint64_t>(value) << index++;
        };
        add(transparentPass);
        add(dynamicBatch);
        add(skinning);
        add(morphing);
        add(instancing);
        add(instancingColor);
        add(instanceLightmap);
        add(screenSpace);
        static_assert(kForwardDrawBitCount == 8, "one bit per draw flag above");
        return bits | (forwardFrameBits() << kForwardDrawBitCount);
    }

    bool ProgramLibrary::forwardShaderResolved(const Material* material, const uint64_t frameBits) const
    {
        if (!material) {
            return false;
        }
        const uint64_t version = material->uniformsVersion();
        for (const auto& memo : material->forwardShaderMemo()) {
            if (memo.library == _serial && memo.materialVersion == version &&
                (memo.state >> kForwardDrawBitCount) == frameBits) {
                return true;
            }
        }
        return false;
    }

    std::shared_ptr<Shader> ProgramLibrary::getForwardShader(const Material* material, const bool transparentPass,
        const bool dynamicBatch, const bool skinning, const bool morphing,
        const bool instancing, const bool instancingColor, const bool instanceLightmap, const bool screenSpace)
    {
        if (!_device) {
            return nullptr;
        }
        if (!material) {
            return resolveForwardShader(material, transparentPass, dynamicBatch, skinning, morphing,
                instancing, instancingColor, instanceLightmap, screenSpace);
        }

        // The resolution is a function of the material (as of its version), the draw's
        // flags, the frame switches and the chunk registry. With all four as they were
        // the last time, the answer is the one the material remembers.
        const uint64_t state = forwardStateBits(transparentPass, dynamicBatch, skinning, morphing,
            instancing, instancingColor, instanceLightmap, screenSpace);
        const uint64_t version = material->uniformsVersion();
        const uint64_t chunksHash = _chunks.hash();
        for (const auto& memo : material->forwardShaderMemo()) {
            if (memo.library == _serial && memo.materialVersion == version && memo.state == state &&
                memo.chunksHash == chunksHash) {
                if (auto shader = memo.shader.lock()) {
#ifndef NDEBUG
                    // A memo that disagrees with the full resolution means an input is
                    // missing from the key: a material mutator that skipped
                    // markUniformsDirty(), or a frame switch not in forwardStateBits.
                    const auto resolved = resolveForwardShader(material, transparentPass, dynamicBatch,
                        skinning, morphing, instancing, instancingColor, instanceLightmap, screenSpace);
                    if (resolved != shader) {
                        spdlog::error("ProgramLibrary: the forward shader remembered for material '{}' is not "
                            "the one its state resolves to", material->name());
                        assert(resolved == shader && "stale forward shader memo");
                        return resolved;
                    }
#endif
                    return shader;
                }
            }
        }

        auto shader = resolveForwardShader(material, transparentPass, dynamicBatch, skinning, morphing,
            instancing, instancingColor, instanceLightmap, screenSpace);
        if (shader) {
            auto& memo = material->nextForwardShaderMemo();
            memo.library = _serial;
            memo.materialVersion = version;
            memo.state = state;
            memo.chunksHash = chunksHash;
            memo.shader = shader;
        }
        return shader;
    }

    std::shared_ptr<Shader> ProgramLibrary::resolveForwardShader(const Material* material, const bool transparentPass,
        const bool dynamicBatch, const bool skinning, const bool morphing,
        const bool instancing, const bool instancingColor, const bool instanceLightmap, const bool screenSpace)
    {
        const ShaderVariantOptions options = buildForwardVariantOptions(material, transparentPass, dynamicBatch,
            skinning, morphing, instancing, instancingColor, instanceLightmap, screenSpace);
        const std::string programName = resolveProgramName(options);
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
        ++_forwardVariantsCreated;
        auto shader = buildForwardShaderVariant(programName, options, variantId, material);
        if (!shader) {
            spdlog::error("Failed to build shader variant '{}' (id={:#x}, localShadows={}, shadows={}, envAtlas={})",
                programName, variantId, options.localShadows, options.shadowMapping, options.envAtlas);
        }
        _forwardShaderCache[key] = shader;
        return shader;
    }

    void ProgramLibrary::prepareDepthOnlyShaders(const bool vsm)
    {
        const size_t index = vsm ? 1 : 0;
        if (_depthOnlyPrepared[index] && _depthOnlyPreparedChunks[index] == _chunks.hash()) {
            return;
        }
        _depthOnlyPrepared[index] = true;
        _depthOnlyPreparedChunks[index] = _chunks.hash();
        (void)getShadowShader(nullptr, false, false, false, false, false, vsm);
        (void)getShadowShader(nullptr, true, false, false, false, false, vsm);
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
        const auto* stdMat = dynamic_cast<const StandardMaterial*>(material);

        ShaderVariantOptions options{};
        options.skybox = false;
        options.transparentPass = false;
        // The shadow pass runs the material's opacity frontend before writing depth.
        // Without it a masked material — foliage,
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
