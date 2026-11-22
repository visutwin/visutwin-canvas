// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// glTF parser: materials and their KHR_materials_* extensions, material variants.
#include "glbParserInternal.h"

#include "glbParser.h"
#include "framework/parsers/packedVertex.h"

#include "framework/components/light/lightComponent.h"

#include <algorithm>
#include <atomic>
#include <numbers>
#include <mutex>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/shape/boundingBox.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/math/vector3.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/textureMips.h"
#include "platform/graphics/textureUtils.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/gsplat/gsplatResource.h"
#include "scene/materials/standardMaterial.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    using namespace gltf;

    /**
     * Apply KHR_materials_transmission / _ior / _volume / _dispersion to a
     * StandardMaterial. Volume attenuation feeds the Beer-law transmittance;
     * dispersion feeds the per-channel refraction in the dynamic grab path.
     *
     * A material carrying transmission OR volume
     * is made blended and switched to dynamic (grab-pass) refraction: without both it
     * renders in the opaque pass and refracts only the environment. Blending
     * is setTransparent(true), NOT setAlphaMode(BLEND), which would also
     * turn depth writes off.
     *
     * DEVIATIONS: `ior` is stored as an IOR where upstream stores 1 / ior (see
     * StandardMaterial::refractionIndex). The attenuation colour is stored LINEAR, as
     * glTF authors it; upstream gamma-encodes it and decodes it again on upload, so
     * the shader sees the same value. KHR_texture_transform on the transmission and
     * thickness textures is not applied: those maps have no transform of their own.
     */
    static void applyVolumeExtensions(const tinygltf::Material& srcMaterial, StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        const auto readNumber = [](const tinygltf::Value& v, const char* key, float fallback) {
            if (v.Has(key)) {
                const auto n = v.Get(key);
                if (n.IsNumber()) return static_cast<float>(n.GetNumberAsDouble());
            }
            return fallback;
        };
        const auto textureIndex = [](const tinygltf::Value& v, const char* key) {
            if (v.Has(key)) {
                const auto t = v.Get(key);
                if (t.IsObject() && t.Has("index")) return t.Get("index").GetNumberAsInt();
            }
            return -1;
        };
        const auto makeRefractive = [material]() {
            material->setTransparent(true);
            material->setUseDynamicRefraction(true);
        };

        if (const auto it = srcMaterial.extensions.find("KHR_materials_transmission");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            makeRefractive();
            material->setTransmissionFactor(readNumber(it->second, "transmissionFactor", 0.0f));
            if (const int idx = textureIndex(it->second, "transmissionTexture"); idx >= 0) {
                if (const auto tex = getOrCreateTexture(idx)) {
                    material->setRefractionMap(tex.get());
                    material->setRefractionMapChannel(MapChannel::MAP_CHANNEL_R);
                }
            }
        }

        if (const auto it = srcMaterial.extensions.find("KHR_materials_ior");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            material->setRefractionIndex(readNumber(it->second, "ior", 1.5f));
        }

        if (const auto it = srcMaterial.extensions.find("KHR_materials_volume");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            makeRefractive();
            material->setThickness(readNumber(it->second, "thicknessFactor", 0.0f));
            if (const int idx = textureIndex(it->second, "thicknessTexture"); idx >= 0) {
                if (const auto tex = getOrCreateTexture(idx)) {
                    material->setThicknessMap(tex.get());
                    material->setThicknessMapChannel(MapChannel::MAP_CHANNEL_G);
                }
            }
            material->setAttenuationDistance(readNumber(it->second, "attenuationDistance", 0.0f));
            if (it->second.Has("attenuationColor")) {
                const auto ac = it->second.Get("attenuationColor");
                if (ac.IsArray() && ac.ArrayLen() >= 3) {
                    material->setAttenuationColor(Color(
                        static_cast<float>(ac.Get(0).GetNumberAsDouble()),
                        static_cast<float>(ac.Get(1).GetNumberAsDouble()),
                        static_cast<float>(ac.Get(2).GetNumberAsDouble()), 1.0f));
                }
            }
        }

        if (const auto it = srcMaterial.extensions.find("KHR_materials_dispersion");
            it != srcMaterial.extensions.end() && it->second.IsObject()) {
            material->setDispersion(readNumber(it->second, "dispersion", 0.0f));
        }
    }

    /**
     * Apply KHR_materials_emissive_strength: a linear multiplier on the emissive
     * term, which is how a glTF asset asks for an emitter brighter than white.
     * It lands on emissiveIntensity because StandardMaterial::updateUniforms
     * uploads linear(emissive) * emissiveIntensity — the factor is already
     * linearised there, so the strength must not be gamma-corrected with it.
     */
    static void applyEmissiveStrength(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        const auto it = srcMaterial.extensions.find("KHR_materials_emissive_strength");
        if (it == srcMaterial.extensions.end() || !it->second.IsObject()) {
            return;
        }
        if (const auto& ext = it->second; ext.Has("emissiveStrength")) {
            if (const auto value = ext.Get("emissiveStrength"); value.IsNumber()) {
                material->setEmissiveIntensity(static_cast<float>(value.GetNumberAsDouble()));
            }
        }
    }

    /**
     * Apply KHR_texture_transform to every texture slot that carries one. The
     * extension lives on the texture INFO, not on the material, so each slot is
     * read separately: base colour, metallic-roughness, normal, occlusion and
     * emissive, which are the five StandardMaterial can transform.
     *
     * The UVs are the file's own (v = 0 at the image's top row), and the per-map packing
     * is upstream's, so upstream's conversion applies as it stands: tiling = scale,
     * rotation = -T in degrees, offset = (gx, 1 - sy - gy). The packing's
     *     v' = sin(r) tx u + cos(r) ty v + (1 - ty - oy)
     * then reproduces glTF's [[sx cosT, sy sinT, gx], [-sx sinT, sy cosT, gy]] term by
     * term. Test a change here with a rotation AND an offset rendered against the same
     * transform baked into the mesh UVs: a sign error in the V term is invisible under a
     * pure SCALE, because the two spellings then differ by a whole number of tiles and
     * REPEAT wrapping hides it.
     *
     * These are the STANDARDMATERIAL per-map properties, not the base Material's
     * TextureTransform fields: `StandardMaterial::updateUniforms` pushes its own
     * tiling/offset/rotation into those base fields on every pack, so anything
     * written there directly is overwritten before it reaches the GPU — the same
     * trap as setDiffuse versus setBaseColorFactor.
     *
     * DEVIATION: the extension's own `texCoord` override is ignored. The texture info's texCoord still selects the UV set.
     */
    static void applyTextureTransforms(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        struct Transform
        {
            Vector2 tiling{1.0f, 1.0f};
            Vector2 offset{0.0f, 0.0f};
            float rotation = 0.0f;
        };

        const auto readTransform = [](const tinygltf::ExtensionMap& extensions, Transform& out) -> bool {
            const auto it = extensions.find("KHR_texture_transform");
            if (it == extensions.end() || !it->second.IsObject()) {
                return false;
            }
            const auto& ext = it->second;

            const auto readPair = [&ext](const char* key, float& x, float& y) {
                if (!ext.Has(key)) {
                    return;
                }
                if (const auto array = ext.Get(key); array.IsArray() && array.ArrayLen() >= 2) {
                    if (const auto v0 = array.Get(0); v0.IsNumber()) x = static_cast<float>(v0.GetNumberAsDouble());
                    if (const auto v1 = array.Get(1); v1.IsNumber()) y = static_cast<float>(v1.GetNumberAsDouble());
                }
            };

            float offsetX = 0.0f, offsetY = 0.0f, scaleX = 1.0f, scaleY = 1.0f;
            readPair("offset", offsetX, offsetY);
            readPair("scale", scaleX, scaleY);

            float rotation = 0.0f;
            if (ext.Has("rotation")) {
                if (const auto value = ext.Get("rotation"); value.IsNumber()) {
                    rotation = static_cast<float>(value.GetNumberAsDouble());
                }
            }

            constexpr float RAD_TO_DEG = 180.0f / 3.14159265358979323846f;
            out.tiling = Vector2(scaleX, scaleY);
            out.offset = Vector2(offsetX, 1.0f - scaleY - offsetY);
            out.rotation = -rotation * RAD_TO_DEG;
            return true;
        };

        Transform transform;
        const auto& pbr = srcMaterial.pbrMetallicRoughness;
        if (readTransform(pbr.baseColorTexture.extensions, transform)) {
            material->setDiffuseMapTiling(transform.tiling);
            material->setDiffuseMapOffset(transform.offset);
            material->setDiffuseMapRotation(transform.rotation);
        }
        if (readTransform(pbr.metallicRoughnessTexture.extensions, transform)) {
            material->setMetalnessMapTiling(transform.tiling);
            material->setMetalnessMapOffset(transform.offset);
            material->setMetalnessMapRotation(transform.rotation);
        }
        if (readTransform(srcMaterial.normalTexture.extensions, transform)) {
            material->setNormalMapTiling(transform.tiling);
            material->setNormalMapOffset(transform.offset);
            material->setNormalMapRotation(transform.rotation);
        }
        if (readTransform(srcMaterial.occlusionTexture.extensions, transform)) {
            material->setAoMapTiling(transform.tiling);
            material->setAoMapOffset(transform.offset);
            material->setAoMapRotation(transform.rotation);
        }
        if (readTransform(srcMaterial.emissiveTexture.extensions, transform)) {
            material->setEmissiveMapTiling(transform.tiling);
            material->setEmissiveMapOffset(transform.offset);
            material->setEmissiveMapRotation(transform.rotation);
        }
    }

    /**
     * Apply KHR_materials_clearcoat to a StandardMaterial. glTF stores coat
     * roughness while the material stores gloss, so the factor routes through
     * setClearCoatGloss + setClearCoatGlossInvert(true). The intensity map is
     * read from R and the roughness map from G, as the extension stores them.
     */
    static void applyClearcoat(
        const tinygltf::Material& srcMaterial,
        StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        const auto it = srcMaterial.extensions.find("KHR_materials_clearcoat");
        if (it == srcMaterial.extensions.end() || !it->second.IsObject()) {
            return;
        }
        const auto& cc = it->second;

        const auto readNumber = [&cc](const char* key, const float fallback) {
            if (cc.Has(key)) {
                const auto n = cc.Get(key);
                if (n.IsNumber()) return static_cast<float>(n.GetNumberAsDouble());
            }
            return fallback;
        };
        const auto textureIndex = [&cc](const char* key) {
            if (cc.Has(key)) {
                const auto t = cc.Get(key);
                if (t.IsObject() && t.Has("index")) return t.Get("index").GetNumberAsInt();
            }
            return -1;
        };

        // A deliberately reproduced upstream quirk: the coat's strength is a QUARTER of the
        // file's clearcoatFactor (upstream calls it a temporary workaround for matching the
        // glTF sample viewer's clear-coat look). Passed through unscaled, every glTF coat drew
        // several times stronger than upstream's: ClearCoatTest's coated column added 17
        // counts of environment reflection where upstream adds 6.5.
        material->setClearCoat(readNumber("clearcoatFactor", 0.0f) * 0.25f);
        material->setClearCoatGloss(readNumber("clearcoatRoughnessFactor", 0.0f));
        material->setClearCoatGlossInvert(true);

        if (const int idx = textureIndex("clearcoatTexture"); idx >= 0) {
            if (const auto tex = getOrCreateTexture(idx)) {
                material->setClearCoatMap(tex.get());
                material->setClearCoatMapChannel(MapChannel::MAP_CHANNEL_R);
            }
        }
        if (const int idx = textureIndex("clearcoatRoughnessTexture"); idx >= 0) {
            if (const auto tex = getOrCreateTexture(idx)) {
                material->setClearCoatGlossMap(tex.get());
                material->setClearCoatGlossMapChannel(MapChannel::MAP_CHANNEL_G);
            }
        }
        if (const int idx = textureIndex("clearcoatNormalTexture"); idx >= 0) {
            if (const auto tex = getOrCreateTexture(idx)) material->setClearCoatNormalMap(tex.get());
            // The texture info's scale scales the coat normal map's XY, as the base
            // normalTexture.scale does the base normal map (spec default 1).
            const auto info = cc.Get("clearcoatNormalTexture");
            material->setClearCoatBumpiness(
                info.Has("scale") && info.Get("scale").IsNumber()
                    ? static_cast<float>(info.Get("scale").GetNumberAsDouble()) : 1.0f);
        }
    }

    // Readers for a material extension object. A missing or mistyped key answers
    // the fallback, which is the extension's spec default at every call site.
    static float extensionNumber(const tinygltf::Value& ext, const char* key, const float fallback)
    {
        if (ext.Has(key)) {
            if (const auto& v = ext.Get(key); v.IsNumber()) {
                return static_cast<float>(v.GetNumberAsDouble());
            }
        }
        return fallback;
    }

    static Color extensionColor(const tinygltf::Value& ext, const char* key, const Color& fallback)
    {
        if (ext.Has(key)) {
            const auto& v = ext.Get(key);
            if (v.IsArray() && v.ArrayLen() >= 3 && v.Get(0).IsNumber() && v.Get(1).IsNumber() && v.Get(2).IsNumber()) {
                return Color(static_cast<float>(v.Get(0).GetNumberAsDouble()),
                    static_cast<float>(v.Get(1).GetNumberAsDouble()),
                    static_cast<float>(v.Get(2).GetNumberAsDouble()), 1.0f);
            }
        }
        return fallback;
    }

    // DEVIATION: the sheen and iridescence extensions allow TEXTURES, and this engine's
    // material has no sheen or iridescence maps (the specular and anisotropy textures ARE
    // read). The factors are applied; a texture is ignored with one warning per extension and
    // process, so the file still loads.
    static void warnIgnoredExtensionTextures(const tinygltf::Value& ext, const char* extension,
        std::initializer_list<const char*> textureKeys)
    {
        static std::mutex mutex;
        static std::set<std::string> warned;
        for (const char* key : textureKeys) {
            if (!ext.Has(key)) {
                continue;
            }
            const std::lock_guard lock(mutex);
            if (warned.insert(extension).second) {
                spdlog::warn("GLB: {} textures are not supported; '{}' and its siblings are ignored, "
                    "the extension's factors still apply", extension, key);
            }
            return;
        }
    }

    static const tinygltf::Value* materialExtension(const tinygltf::Material& srcMaterial, const char* name)
    {
        const auto it = srcMaterial.extensions.find(name);
        return it != srcMaterial.extensions.end() && it->second.IsObject() ? &it->second : nullptr;
    }

    /**
     * KHR_materials_sheen: the colour factor is
     * linear in the file and stored gamma-encoded (both shaders decode it), the
     * roughness factor is the sheen roughness. An absent colour factor is the spec's
     * default of black: no sheen.
     */
    static void applySheen(const tinygltf::Material& srcMaterial, StandardMaterial* material)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_sheen");
        if (!ext) {
            return;
        }
        Color color = extensionColor(*ext, "sheenColorFactor", Color(0.0f, 0.0f, 0.0f, 1.0f));
        color.gamma();
        material->setSheenColor(color);
        material->setSheenRoughness(extensionNumber(*ext, "sheenRoughnessFactor", 0.0f));
        warnIgnoredExtensionTextures(*ext, "KHR_materials_sheen", {"sheenColorTexture", "sheenRoughnessTexture"});
    }

    /**
     * KHR_materials_specular, for a metallic-rough
     * material: the colour factor tints the non-metal F0 (stored gamma-encoded),
     * the factor scales it, and their textures (colour rgb, factor alpha) scale them per
     * texel; metals are untouched.
     */
    static void applySpecularExtension(const tinygltf::Material& srcMaterial, StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_specular");
        if (!ext || !material->useMetalness()) {
            return;
        }
        Color color = extensionColor(*ext, "specularColorFactor", Color(1.0f, 1.0f, 1.0f, 1.0f));
        color.gamma();
        material->setSpecular(color);
        material->setUseMetalnessSpecularColor(true);
        material->setSpecularityFactor(extensionNumber(*ext, "specularFactor", 1.0f));
        // The textures: specularTexture's alpha scales the factor, specularColorTexture's rgb
        // (sRGB) tints the colour, each on the UV set it names.
        const auto textureOf = [&](const char* name, int& uv) -> std::shared_ptr<Texture> {
            if (!ext->Has(name)) {
                return nullptr;
            }
            const auto info = ext->Get(name);
            const int index = info.IsObject() && info.Has("index") ? info.Get("index").GetNumberAsInt() : -1;
            uv = info.IsObject() && info.Has("texCoord") && info.Get("texCoord").GetNumberAsInt() == 1 ? 1 : 0;
            return index >= 0 ? getOrCreateTexture(index) : nullptr;
        };
        int uv = 0;
        if (const auto texture = textureOf("specularTexture", uv)) {
            material->setSpecularityFactorMap(texture.get());
            material->setSpecularityFactorMapUv(uv);
        }
        if (const auto texture = textureOf("specularColorTexture", uv)) {
            material->setSpecularColorMap(texture.get());
            material->setSpecularColorMapUv(uv);
        }
    }

    /**
     * KHR_materials_iridescence: the factor, the thin film's IOR (stored as an IOR
     * here, see StandardMaterial) and its thickness. Without a thickness texture the
     * spec's film is the MAXIMUM thickness; the minimum only matters to the texture.
     */
    static void applyIridescence(const tinygltf::Material& srcMaterial, StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_iridescence");
        if (!ext) {
            return;
        }
        material->setIridescenceIntensity(extensionNumber(*ext, "iridescenceFactor", 0.0f));
        material->setIridescenceIOR(extensionNumber(*ext, "iridescenceIor", 1.3f));
        material->setIridescenceThicknessMin(extensionNumber(*ext, "iridescenceThicknessMinimum", 100.0f));
        material->setIridescenceThicknessMax(extensionNumber(*ext, "iridescenceThicknessMaximum", 400.0f));
        // The maps, each on the texture coordinate set the file names: the intensity in r, the
        // thickness in g (both channels as the extension defines them).
        const auto textureOf = [&](const char* name, int& texCoord) -> std::shared_ptr<Texture> {
            if (!ext->Has(name)) {
                return nullptr;
            }
            const auto info = ext->Get(name);
            const int index = info.IsObject() && info.Has("index") ? info.Get("index").GetNumberAsInt() : -1;
            texCoord = info.IsObject() && info.Has("texCoord") ? info.Get("texCoord").GetNumberAsInt() : 0;
            return index >= 0 ? getOrCreateTexture(index) : nullptr;
        };
        int texCoord = 0;
        if (const auto texture = textureOf("iridescenceTexture", texCoord)) {
            material->setIridescenceMap(texture.get());
            material->setIridescenceMapUv(texCoord == 1 ? 1 : 0);
        }
        if (const auto texture = textureOf("iridescenceThicknessTexture", texCoord)) {
            material->setIridescenceThicknessMap(texture.get());
            material->setIridescenceThicknessMapUv(texCoord == 1 ? 1 : 0);
        }
    }

    /**
     * KHR_materials_diffuse_transmission: the fraction of the diffuse light passing through the
     * surface and the colour it is tinted by (stored in gamma space, as every material colour),
     * with their textures on the UV sets the file names: the fraction in the texture's ALPHA,
     * the colour in its rgb (sRGB).
     */
    static void applyDiffuseTransmission(const tinygltf::Material& srcMaterial, StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_diffuse_transmission");
        if (!ext) {
            return;
        }
        material->setDiffuseTransmission(extensionNumber(*ext, "diffuseTransmissionFactor", 0.0f));
        Color color = extensionColor(*ext, "diffuseTransmissionColorFactor", Color(1.0f, 1.0f, 1.0f, 1.0f));
        color.gamma();
        material->setDiffuseTransmissionColor(color);
        const auto textureOf = [&](const char* name, int& uv) -> std::shared_ptr<Texture> {
            if (!ext->Has(name)) {
                return nullptr;
            }
            const auto info = ext->Get(name);
            const int index = info.IsObject() && info.Has("index") ? info.Get("index").GetNumberAsInt() : -1;
            uv = info.IsObject() && info.Has("texCoord") && info.Get("texCoord").GetNumberAsInt() == 1 ? 1 : 0;
            return index >= 0 ? getOrCreateTexture(index) : nullptr;
        };
        int uv = 0;
        if (const auto texture = textureOf("diffuseTransmissionTexture", uv)) {
            material->setDiffuseTransmissionMap(texture.get());
            material->setDiffuseTransmissionMapChannel(MapChannel::MAP_CHANNEL_A);
            material->setDiffuseTransmissionMapUv(uv);
        }
        if (const auto texture = textureOf("diffuseTransmissionColorTexture", uv)) {
            material->setDiffuseTransmissionColorMap(texture.get());
            material->setDiffuseTransmissionColorMapUv(uv);
        }
    }

    /**
     * KHR_materials_anisotropy: the strength, the rotation (radians in the file, degrees on
     * the material, from the tangent toward the bitangent) and the anisotropy texture
     * (direction in rg, strength in b) on its texture coordinate set.
     */
    static void applyAnisotropyExtension(const tinygltf::Material& srcMaterial, StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        const auto* ext = materialExtension(srcMaterial, "KHR_materials_anisotropy");
        if (!ext) {
            return;
        }
        // Anisotropy is a GGX lobe, so the extension turns GGX direct specular on.
        material->setEnableGGXSpecular(true);
        material->setAnisotropy(extensionNumber(*ext, "anisotropyStrength", 0.0f));
        material->setAnisotropyRotation(static_cast<float>(
            extensionNumber(*ext, "anisotropyRotation", 0.0f) * 180.0 / std::numbers::pi));
        if (ext->Has("anisotropyTexture")) {
            const auto info = ext->Get("anisotropyTexture");
            const int index = info.IsObject() && info.Has("index") ? info.Get("index").GetNumberAsInt() : -1;
            if (const auto texture = index >= 0 ? getOrCreateTexture(index) : nullptr) {
                material->setAnisotropyMap(texture.get());
                const int texCoord = info.Has("texCoord") ? info.Get("texCoord").GetNumberAsInt() : 0;
                material->setAnisotropyMapUv(texCoord == 1 ? 1 : 0);
            }
        }
    }

    /**
     * Apply KHR_materials_pbrSpecularGlossiness extension to a StandardMaterial.
     * Maps diffuseTexture → baseColorTexture and specular/glossiness factors.
     */
    static void applySpecularGlossiness(
        const tinygltf::Material& srcMaterial,
        StandardMaterial* material,
        const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture)
    {
        auto sgIt = srcMaterial.extensions.find("KHR_materials_pbrSpecularGlossiness");
        if (sgIt == srcMaterial.extensions.end() || !sgIt->second.IsObject()) return;

        const auto& sg = sgIt->second;

        // diffuseFactor → the diffuse colour and the opacity (its 4th component), white
        // and opaque when absent. Opacity only: whether the material blends is the
        // file's alphaMode, which createGltfMaterial has already applied. An alpha
        // below 1 does not turn an OPAQUE material into a blended one, and a BLEND
        // material keeps exactly the alpha the file gives it.
        Color diffColor(1.0f, 1.0f, 1.0f, 1.0f);
        if (sg.Has("diffuseFactor")) {
            const auto df = sg.Get("diffuseFactor");
            if (df.IsArray() && df.ArrayLen() >= 3) {
                const auto component = [&df](const int i) {
                    return df.Get(i).IsNumber() ? static_cast<float>(df.Get(i).GetNumberAsDouble()) : 1.0f;
                };
                diffColor = Color(component(0), component(1), component(2), df.ArrayLen() >= 4 ? component(3) : 1.0f);
            }
        }
        material->setBaseColorFactor(diffColor);
        Color gammaColor(diffColor);
        gammaColor.gamma();
        material->setDiffuse(gammaColor);
        material->setOpacity(diffColor.a);

        // diffuseTexture → baseColorTexture
        if (sg.Has("diffuseTexture")) {
            const auto dt = sg.Get("diffuseTexture");
            if (dt.IsObject() && dt.Has("index")) {
                const int texIdx = dt.Get("index").GetNumberAsInt();
                if (auto tex = getOrCreateTexture(texIdx)) {
                    // Set on BOTH base Material and StandardMaterial paths
                    material->setBaseColorTexture(tex.get());
                    material->setHasBaseColorTexture(true);
                    material->setDiffuseMap(tex.get());
                    if (dt.Has("texCoord"))
                        material->setBaseColorUvSet(dt.Get("texCoord").GetNumberAsInt());
                } else {
                    spdlog::warn("GLB material '{}': spec-gloss diffuse texture {} could not be created",
                        material->name(), texIdx);
                }
            }
        }

        // The specular workflow of the KHR_materials_pbrSpecularGlossiness
        // extension: useMetalness off, `specular` stored in sRGB (the factor
        // is linear, so it is gamma-encoded here and linearised again on upload), and
        // the extension's defaults — white specular, glossiness 1 — when a factor is
        // absent. Leaving the specular black would render no specular at all.
        material->setUseMetalness(false);
        material->setMetalness(0.0f);
        material->setMetallicFactor(0.0f);

        // specularGlossinessTexture: rgb = specular color (sRGB), a = glossiness.
        // Bound at the metal-rough slot (3); the SPEC_GLOSS variant reinterprets it.
        if (sg.Has("specularGlossinessTexture")) {
            auto sgt = sg.Get("specularGlossinessTexture");
            if (sgt.IsObject() && sgt.Has("index")) {
                int texIdx = sgt.Get("index").GetNumberAsInt();
                if (auto tex = getOrCreateTexture(texIdx)) {
                    material->setSpecGlossMap(tex.get());
                }
            }
        }

        Color specular(1.0f, 1.0f, 1.0f, 1.0f);
        if (sg.Has("specularFactor")) {
            auto sf = sg.Get("specularFactor");
            if (sf.IsArray() && sf.ArrayLen() >= 3) {
                specular = Color(
                    static_cast<float>(sf.Get(0).GetNumberAsDouble()),
                    static_cast<float>(sf.Get(1).GetNumberAsDouble()),
                    static_cast<float>(sf.Get(2).GetNumberAsDouble()), 1.0f);
            }
        }
        specular.gamma();
        material->setSpecular(specular);

        float gloss = 1.0f;
        if (sg.Has("glossinessFactor")) {
            auto gf = sg.Get("glossinessFactor");
            if (gf.IsNumber()) {
                gloss = static_cast<float>(gf.GetNumberAsDouble());
            }
        }
        material->setGloss(gloss);
        material->setRoughnessFactor(1.0f - gloss);
    }
    namespace gltf
    {
        // One glTF material, for EVERY load path: the synchronous parse(), createFromModel
        // and createFromPrepared (the two asynchronous ones). Never give a path its own
        // copy: copies drift, and nothing notices because no example loads asynchronously.
        // `textureCount`, when given, counts the core textures bound.
        std::shared_ptr<StandardMaterial> createGltfMaterial(const tinygltf::Material& srcMaterial,
            const std::function<std::shared_ptr<Texture>(int)>& getOrCreateTexture, size_t* textureCount)
        {
            auto material = std::make_shared<StandardMaterial>();
            material->setName(srcMaterial.name.empty() ? "glTF-material" : srcMaterial.name);

            // A core texture that the model names but that could not be created is worth a
            // warning on every path; before, only one async path said so.
            const auto bindTexture = [&](const int textureIndex, const char* what) -> std::shared_ptr<Texture> {
                auto texture = getOrCreateTexture(textureIndex);
                if (texture) {
                    if (textureCount) {
                        ++*textureCount;
                    }
                } else {
                    spdlog::warn("GLB material '{}': {} texture {} could not be created",
                        material->name(), what, textureIndex);
                }
                return texture;
            };

            const auto& pbr = srcMaterial.pbrMetallicRoughness;
            if (pbr.baseColorFactor.size() == 4) {
                const Color baseColor(
                    static_cast<float>(pbr.baseColorFactor[0]),
                    static_cast<float>(pbr.baseColorFactor[1]),
                    static_cast<float>(pbr.baseColorFactor[2]),
                    static_cast<float>(pbr.baseColorFactor[3]));
                material->setBaseColorFactor(baseColor);
                // Also set StandardMaterial diffuse + opacity so that updateUniforms() uses
                // the glTF base color (not white default). glTF baseColorFactor is linear;
                // StandardMaterial.diffuse expects sRGB (the shader applies srgbToLinear).
                Color diffuseColor(baseColor);
                diffuseColor.gamma();
                material->setDiffuse(diffuseColor);
                material->setOpacity(baseColor.a);
            }
            const float metallicFactor = static_cast<float>(pbr.metallicFactor);
            const float roughnessFactor = static_cast<float>(pbr.roughnessFactor);
            material->setMetallicFactor(metallicFactor);
            material->setRoughnessFactor(roughnessFactor);
            // StandardMaterial convention: gloss = 1 - roughness (glossInvert=false).
            material->setMetalness(metallicFactor);
            material->setGloss(1.0f - roughnessFactor);
            // glTF metallic-roughness: useMetalness is set for every glTF material.
            material->setUseMetalness(true);

            if (!srcMaterial.alphaMode.empty()) {
                if (srcMaterial.alphaMode == "BLEND") {
                    material->setAlphaMode(AlphaMode::BLEND);
                    material->setTransparent(true);
                } else if (srcMaterial.alphaMode == "MASK") {
                    material->setAlphaMode(AlphaMode::MASK);
                    material->setTransparent(false);
                } else {
                    material->setAlphaMode(AlphaMode::OPAQUE);
                    material->setTransparent(false);
                }
            }
            material->setCullMode(srcMaterial.doubleSided ? CullMode::CULLFACE_NONE : CullMode::CULLFACE_BACK);
            material->setTwoSidedLighting(srcMaterial.doubleSided);
            material->setAlphaCutoff(static_cast<float>(srcMaterial.alphaCutoff));

            applySpecularGlossiness(srcMaterial, material.get(), getOrCreateTexture);
            applyVolumeExtensions(srcMaterial, material.get(), getOrCreateTexture);
            applyClearcoat(srcMaterial, material.get(), getOrCreateTexture);
            applySheen(srcMaterial, material.get());
            applySpecularExtension(srcMaterial, material.get(), getOrCreateTexture);
            applyIridescence(srcMaterial, material.get(), getOrCreateTexture);
            applyDiffuseTransmission(srcMaterial, material.get(), getOrCreateTexture);
            applyAnisotropyExtension(srcMaterial, material.get(), getOrCreateTexture);
            applyEmissiveStrength(srcMaterial, material.get());
            applyTextureTransforms(srcMaterial, material.get());

            if (pbr.baseColorTexture.index >= 0) {
                if (auto texture = bindTexture(pbr.baseColorTexture.index, "baseColor")) {
                    material->setBaseColorTexture(texture.get());
                    material->setHasBaseColorTexture(true);
                    material->setBaseColorUvSet(pbr.baseColorTexture.texCoord);
                }
            }
            if (srcMaterial.normalTexture.index >= 0) {
                if (auto texture = bindTexture(srcMaterial.normalTexture.index, "normal")) {
                    material->setNormalTexture(texture.get());
                    material->setHasNormalTexture(true);
                    material->setNormalUvSet(srcMaterial.normalTexture.texCoord);
                }
                material->setNormalScale(static_cast<float>(srcMaterial.normalTexture.scale));
                material->setBumpiness(static_cast<float>(srcMaterial.normalTexture.scale));
            }
            if (pbr.metallicRoughnessTexture.index >= 0) {
                if (auto texture = bindTexture(pbr.metallicRoughnessTexture.index, "metallicRoughness")) {
                    material->setMetallicRoughnessTexture(texture.get());
                    material->setHasMetallicRoughnessTexture(true);
                    material->setMetallicRoughnessUvSet(pbr.metallicRoughnessTexture.texCoord);
                }
            }
            if (srcMaterial.occlusionTexture.index >= 0) {
                if (auto texture = bindTexture(srcMaterial.occlusionTexture.index, "occlusion")) {
                    material->setOcclusionTexture(texture.get());
                    material->setHasOcclusionTexture(true);
                    material->setOcclusionUvSet(srcMaterial.occlusionTexture.texCoord);
                }
                material->setOcclusionStrength(static_cast<float>(srcMaterial.occlusionTexture.strength));
            }
            if (srcMaterial.emissiveFactor.size() == 3) {
                // glTF emissiveFactor is linear; material.emissive expects sRGB.
                Color emissiveColor(
                    static_cast<float>(srcMaterial.emissiveFactor[0]),
                    static_cast<float>(srcMaterial.emissiveFactor[1]),
                    static_cast<float>(srcMaterial.emissiveFactor[2]),
                    1.0f);
                emissiveColor.gamma();
                material->setEmissiveFactor(emissiveColor);
                // StandardMaterial::updateUniforms overrides emissiveFactor with
                // _emissive * _emissiveIntensity — mirror into the authoritative slot.
                material->setEmissive(emissiveColor);
            }
            if (srcMaterial.emissiveTexture.index >= 0) {
                if (auto texture = bindTexture(srcMaterial.emissiveTexture.index, "emissive")) {
                    material->setEmissiveTexture(texture.get());
                    material->setHasEmissiveTexture(true);
                    material->setEmissiveUvSet(srcMaterial.emissiveTexture.texCoord);
                }
            }

            const bool isUnlit = srcMaterial.extensions.contains("KHR_materials_unlit");

            uint64_t variant = 1;
            if (material->hasBaseColorTexture()) {
                variant |= (1ull << 1);
            }
            if (material->hasNormalTexture()) {
                variant |= (1ull << 4);
            }
            if (material->hasMetallicRoughnessTexture()) {
                variant |= (1ull << 5);
            }
            if (material->hasOcclusionTexture()) {
                variant |= (1ull << 6);
            }
            if (material->hasEmissiveTexture()) {
                variant |= (1ull << 7);
            }
            if (material->alphaMode() == AlphaMode::BLEND) {
                variant |= (1ull << 2);
            } else if (material->alphaMode() == AlphaMode::MASK) {
                variant |= (1ull << 3);
            }
            if (isUnlit) {
                variant |= (1ull << 32);  // VT_FEATURE_UNLIT
            }
            material->setShaderVariantKey(variant);
            return material;
        }

        // The material a primitive with no material gets: an empty glTF material, so the
        // specification's defaults (white, metallic 1, roughness 1: a rough white metal that
        // reflects the environment), the same path as every other material.
        std::shared_ptr<StandardMaterial> createDefaultGltfMaterial()
        {
            tinygltf::Material empty;
            empty.name = "defaultGlbMaterial";
            return createGltfMaterial(empty, [](int) { return std::shared_ptr<Texture>(); });
        }

        // KHR_materials_variants on a primitive: each
        // mapping gives one material to a list of variant indices.
        std::vector<std::pair<int, int>> primitiveVariantMaterials(const tinygltf::Primitive& primitive)
        {
            std::vector<std::pair<int, int>> result;
            const auto ext = primitive.extensions.find("KHR_materials_variants");
            if (ext == primitive.extensions.end() || !ext->second.IsObject() || !ext->second.Has("mappings")) {
                return result;
            }
            const auto& mappings = ext->second.Get("mappings");
            for (size_t m = 0; mappings.IsArray() && m < mappings.ArrayLen(); ++m) {
                const auto& mapping = mappings.Get(static_cast<int>(m));
                if (!mapping.IsObject() || !mapping.Has("material") || !mapping.Has("variants")) {
                    continue;
                }
                const int material = mapping.Get("material").GetNumberAsInt();
                const auto& variants = mapping.Get("variants");
                for (size_t v = 0; variants.IsArray() && v < variants.ArrayLen(); ++v) {
                    result.emplace_back(variants.Get(static_cast<int>(v)).GetNumberAsInt(), material);
                }
            }
            return result;
        }

        // A point cloud draws unlit with its vertex colours, from a COPY of its glTF
        // material so the point bits cannot leak onto triangle meshes sharing it. An
        // animated model's clouds glow additively without writing depth; the merged
        // static cloud stays opaque.
        std::shared_ptr<Material> pointCloudMaterial(const std::vector<std::shared_ptr<Material>>& materials,
            const int materialIndex, const bool animated)
        {
            const auto& base = (materialIndex >= 0 && materialIndex < static_cast<int>(materials.size()))
                ? materials[static_cast<size_t>(materialIndex)] : materials.front();
            auto material = std::make_shared<StandardMaterial>(*std::static_pointer_cast<StandardMaterial>(base));
            uint64_t variant = material->shaderVariantKey();
            variant |= (1ull << 21);  // VT_FEATURE_VERTEX_COLORS
            variant |= (1ull << 31);  // VT_FEATURE_POINT_SIZE
            if (animated) {
                variant |= (1ull << 32);  // VT_FEATURE_UNLIT
            }
            material->setShaderVariantKey(variant);
            if (animated) {
                material->setTransparent(true);
                material->setBlendState(std::make_shared<BlendState>(BlendState::additiveBlend()));
                material->setDepthState(std::make_shared<DepthState>(DepthState::noWrite()));
            }
            return material;
        }
    }
}
