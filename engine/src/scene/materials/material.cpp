// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#include "material.h"

#include <algorithm>
#include <assert.h>
#include <cmath>
#include <initializer_list>
#include <unordered_map>

#include "platform/graphics/deviceCache.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    DeviceCache defaultMaterialDeviceCache;
    std::unordered_map<GraphicsDevice*, std::shared_ptr<Material>> defaultMaterials;

    namespace
    {
        const Material::ParameterValue* getParam(const Material* material, std::initializer_list<const char*> names)
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

        bool readFloat(const Material::ParameterValue* value, float& out)
        {
            if (!value) {
                return false;
            }
            if (const auto* v = std::get_if<float>(value)) {
                out = *v;
                return true;
            }
            if (const auto* v = std::get_if<int32_t>(value)) {
                out = static_cast<float>(*v);
                return true;
            }
            if (const auto* v = std::get_if<uint32_t>(value)) {
                out = static_cast<float>(*v);
                return true;
            }
            return false;
        }

        bool readInt(const Material::ParameterValue* value, int& out)
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

        bool readColor4(const Material::ParameterValue* value, float out[4])
        {
            if (!value) {
                return false;
            }
            if (const auto* v = std::get_if<Color>(value)) {
                out[0] = v->r;
                out[1] = v->g;
                out[2] = v->b;
                out[3] = v->a;
                return true;
            }
            if (const auto* v = std::get_if<Vector3>(value)) {
                out[0] = v->getX();
                out[1] = v->getY();
                out[2] = v->getZ();
                out[3] = 1.0f;
                return true;
            }
            if (const auto* v = std::get_if<Vector4>(value)) {
                out[0] = v->getX();
                out[1] = v->getY();
                out[2] = v->getZ();
                out[3] = v->getW();
                return true;
            }
            if (const auto* v = std::get_if<float>(value)) {
                out[0] = *v;
                out[1] = *v;
                out[2] = *v;
                out[3] = 1.0f;
                return true;
            }
            return false;
        }

        bool readTexture(const Material::ParameterValue* value, Texture*& out)
        {
            if (!value) {
                return false;
            }
            if (const auto* v = std::get_if<Texture*>(value)) {
                out = *v;
                return true;
            }
            return false;
        }

        // pre-compute 3×2 affine matrix from tiling, offset, rotation.
        // Matches upstream defineUniform() for texture_*MapTransform0/1.
        constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;

        void packTransform(const TextureTransform& t, float row0[4], float row1[4])
        {
            const float cr = std::cos(t.rotation * DEG_TO_RAD);
            const float sr = std::sin(t.rotation * DEG_TO_RAD);
            row0[0] = cr * t.tiling.x;
            row0[1] = -sr * t.tiling.y;
            row0[2] = t.offset.x;
            row0[3] = 0.0f;
            row1[0] = sr * t.tiling.x;
            row1[1] = cr * t.tiling.y;
            row1[2] = 1.0f - t.tiling.y - t.offset.y;
            row1[3] = 0.0f;
        }
    }

    Material::Material()
    {
        _blendState = std::make_shared<BlendState>();
        _depthState = std::make_shared<DepthState>();
    }

    void Material::setAlphaMode(const AlphaMode mode)
    {
        _alphaMode = mode;
        if (mode == AlphaMode::BLEND) {
            // Standard glTF "BLEND": src*srcAlpha + dst*(1-srcAlpha), with depth-write off so
            // overlapping transparent surfaces don't punch holes in each other.
            _blendState = std::make_shared<BlendState>(BlendState::alphaBlend());
            _depthState = std::make_shared<DepthState>(DepthState::noWrite());
            _transparent = true;
        } else {
            // OPAQUE or MASK: no blending, normal depth-write. Reset to defaults.
            _blendState = std::make_shared<BlendState>();
            _depthState = std::make_shared<DepthState>();
            _transparent = false;
        }
    }

    void Material::setParameter(const std::string& name, const ParameterValue& value)
    {
        if (name.empty()) {
            return;
        }
        _parameters[name] = value;
    }

    bool Material::removeParameter(const std::string& name)
    {
        if (name.empty()) {
            return false;
        }
        return _parameters.erase(name) > 0;
    }

    void Material::clearParameters()
    {
        _parameters.clear();
    }

    const Material::ParameterValue* Material::parameter(const std::string& name) const
    {
        const auto it = _parameters.find(name);
        return it == _parameters.end() ? nullptr : &it->second;
    }

    void Material::updateUniforms(MaterialUniforms& uniforms) const
    {
        // Pack typed properties into GPU struct.
        uniforms.baseColor[0] = _baseColorFactor.r;
        uniforms.baseColor[1] = _baseColorFactor.g;
        uniforms.baseColor[2] = _baseColorFactor.b;
        uniforms.baseColor[3] = _baseColorFactor.a;
        // Emissive is authored in sRGB (convention matches PlayCanvas material.emissive and the
        // .gamma() conversion the glTF parser applies to glTF's linear emissiveFactor). The GPU
        // wants linear HDR, and a StandardMaterial subclass may multiply by emissiveIntensity > 1.
        // Linearize FIRST here so the intensity scaling (applied by StandardMaterial::updateUniforms
        // below) happens in linear space — applying pow() to intensity-scaled sRGB blows up to
        // +Inf for bright neon (e.g. 200 * 1.0 → pow(200, 2.2) ≈ 1.7e5, overflowing fp16 targets).
        uniforms.emissiveColor[0] = std::pow(std::max(_emissiveFactor.r, 0.0f), 2.2f);
        uniforms.emissiveColor[1] = std::pow(std::max(_emissiveFactor.g, 0.0f), 2.2f);
        uniforms.emissiveColor[2] = std::pow(std::max(_emissiveFactor.b, 0.0f), 2.2f);
        uniforms.emissiveColor[3] = _emissiveFactor.a;
        uniforms.alphaCutoff = _alphaCutoff;
        uniforms.metallicFactor = _metallicFactor;
        uniforms.roughnessFactor = _roughnessFactor;
        uniforms.normalScale = _normalScale;
        uniforms.occlusionStrength = _occlusionStrength;
        uniforms.occludeSpecularMode = _occludeSpecular;
        uniforms.occludeSpecularIntensity = _occludeSpecularIntensity;
        uniforms.flags = 0u;

        // Allow custom parameter overrides (same alias chains as the original inline code).
        readColor4(getParam(this, {"material_baseColor", "baseColorFactor"}), uniforms.baseColor);
        {
            // Parameter override convention is sRGB input — linearize to match the typed path above.
            float emissiveOverride[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (readColor4(getParam(this, {"material_emissive", "emissiveFactor"}), emissiveOverride)) {
                uniforms.emissiveColor[0] = std::pow(std::max(emissiveOverride[0], 0.0f), 2.2f);
                uniforms.emissiveColor[1] = std::pow(std::max(emissiveOverride[1], 0.0f), 2.2f);
                uniforms.emissiveColor[2] = std::pow(std::max(emissiveOverride[2], 0.0f), 2.2f);
                uniforms.emissiveColor[3] = emissiveOverride[3];
            }
        }
        readFloat(getParam(this, {"material_alphaCutoff", "alphaCutoff"}), uniforms.alphaCutoff);
        readFloat(getParam(this, {"material_metallic", "metallicFactor"}), uniforms.metallicFactor);
        readFloat(getParam(this, {"material_roughness", "roughnessFactor"}), uniforms.roughnessFactor);
        readFloat(getParam(this, {"material_normalScale", "normalScale"}), uniforms.normalScale);
        readFloat(getParam(this, {"material_occlusionStrength", "occlusionStrength"}), uniforms.occlusionStrength);
        readFloat(getParam(this, {"material_occludeSpecularIntensity", "occludeSpecularIntensity"}),
            uniforms.occludeSpecularIntensity);
        {
            int occludeSpecularMode = static_cast<int>(uniforms.occludeSpecularMode);
            readInt(getParam(this, {"material_occludeSpecular", "occludeSpecular"}), occludeSpecularMode);
            occludeSpecularMode = std::clamp(occludeSpecularMode,
                static_cast<int>(SPECOCC_NONE), static_cast<int>(SPECOCC_GLOSSDEPENDENT));
            uniforms.occludeSpecularMode = static_cast<uint32_t>(occludeSpecularMode);
        }

        // Flag bits — matches MaterialData.flags layout in common.metal.
        if (_hasBaseColorTexture) {
            uniforms.flags |= 1u;           // bit 0: hasBaseColorMap
        }
        if (_alphaMode == AlphaMode::MASK) {
            uniforms.flags |= (1u << 1);    // bit 1: alphaTest
        }
        if (_hasNormalTexture) {
            uniforms.flags |= (1u << 2);    // bit 2: hasNormalMap
        }
        if (_cullMode == CullMode::CULLFACE_NONE) {
            uniforms.flags |= (1u << 3);    // bit 3: doubleSided
        }

        // UV set selection bits.
        int baseUvSet = _baseColorUvSet;
        int normalUvSet = _normalUvSet;
        int metallicUvSet = _metallicRoughnessUvSet;
        int occlusionUvSet = _occlusionUvSet;
        int emissiveUvSet = _emissiveUvSet;
        readInt(getParam(this, {"baseColorUvSet"}), baseUvSet);
        readInt(getParam(this, {"normalUvSet"}), normalUvSet);
        readInt(getParam(this, {"metallicRoughnessUvSet"}), metallicUvSet);
        readInt(getParam(this, {"occlusionUvSet"}), occlusionUvSet);
        readInt(getParam(this, {"emissiveUvSet"}), emissiveUvSet);
        if (baseUvSet == 1)     uniforms.flags |= (1u << 4);
        if (normalUvSet == 1)   uniforms.flags |= (1u << 5);
        if (_hasMetallicRoughnessTexture) uniforms.flags |= (1u << 6);
        if (metallicUvSet == 1) uniforms.flags |= (1u << 7);
        if (_isSkybox)          uniforms.flags |= (1u << 8);
        if (_hasOcclusionTexture) uniforms.flags |= (1u << 9);
        if (occlusionUvSet == 1) uniforms.flags |= (1u << 10);
        if (_hasEmissiveTexture) uniforms.flags |= (1u << 11);
        if (emissiveUvSet == 1) uniforms.flags |= (1u << 12);

        int occludeDirect = _occludeDirect ? 1 : 0;
        readInt(getParam(this, {"material_occludeDirect", "occludeDirect"}), occludeDirect);
        if (occludeDirect != 0) uniforms.flags |= (1u << 13);

        // Height/parallax map: flag bit 17.
        Texture* heightTex = nullptr;
        readTexture(getParam(this, {"texture_heightMap"}), heightTex);
        if (heightTex) uniforms.flags |= (1u << 17);
        readFloat(getParam(this, {"material_heightMapFactor", "heightMapFactor"}), uniforms.heightMapFactor);

        // Anisotropy: parameter override.
        readFloat(getParam(this, {"material_anisotropy", "anisotropy"}), uniforms.anisotropy);

        // Transmission/refraction: parameter overrides.
        readFloat(getParam(this, {"material_transmissionFactor", "transmissionFactor"}), uniforms.transmissionFactor);
        readFloat(getParam(this, {"material_refractionIndex", "refractionIndex"}), uniforms.refractionIndex);
        readFloat(getParam(this, {"material_thickness", "thickness"}), uniforms.thickness);

        // Sheen: parameter overrides (KHR_materials_sheen).
        readColor4(getParam(this, {"material_sheenColor", "sheenColor"}), uniforms.sheenColor);
        readFloat(getParam(this, {"material_sheenRoughness", "sheenRoughness"}), uniforms.sheenColor[3]);
        {
            Texture* sheenTex = nullptr;
            readTexture(getParam(this, {"texture_sheenMap"}), sheenTex);
            if (sheenTex) uniforms.flags |= (1u << 18);
        }

        // Iridescence: parameter overrides (KHR_materials_iridescence).
        readFloat(getParam(this, {"material_iridescenceIntensity", "iridescenceIntensity"}), uniforms.iridescenceParams[0]);
        readFloat(getParam(this, {"material_iridescenceIOR", "iridescenceIOR"}), uniforms.iridescenceParams[1]);
        readFloat(getParam(this, {"material_iridescenceThicknessMin", "iridescenceThicknessMin"}), uniforms.iridescenceParams[2]);
        readFloat(getParam(this, {"material_iridescenceThicknessMax", "iridescenceThicknessMax"}), uniforms.iridescenceParams[3]);
        {
            Texture* iriTex = nullptr;
            readTexture(getParam(this, {"texture_iridescenceMap"}), iriTex);
            if (iriTex) uniforms.flags |= (1u << 19);
            Texture* iriThickTex = nullptr;
            readTexture(getParam(this, {"texture_iridescenceThicknessMap"}), iriThickTex);
            if (iriThickTex) uniforms.flags |= (1u << 20);
        }

        // Spec-Gloss: parameter overrides (KHR_materials_pbrSpecularGlossiness).
        readColor4(getParam(this, {"material_specularColor", "specularColor"}), uniforms.specGlossParams);
        readFloat(getParam(this, {"material_glossiness", "glossiness"}), uniforms.specGlossParams[3]);
        {
            Texture* sgTex = nullptr;
            readTexture(getParam(this, {"texture_specGlossMap"}), sgTex);
            if (sgTex) uniforms.flags |= (1u << 21);
        }

        // Detail normals: parameter overrides.
        readFloat(getParam(this, {"material_detailNormalScale", "detailNormalScale"}), uniforms.detailDisplacementParams[0]);
        {
            Texture* detailTex = nullptr;
            readTexture(getParam(this, {"texture_detailNormalMap"}), detailTex);
            if (detailTex) uniforms.flags |= (1u << 22);
        }

        // Displacement: parameter overrides.
        readFloat(getParam(this, {"material_displacementScale", "displacementScale"}), uniforms.detailDisplacementParams[1]);
        readFloat(getParam(this, {"material_displacementBias", "displacementBias"}), uniforms.detailDisplacementParams[2]);
        {
            Texture* dispTex = nullptr;
            readTexture(getParam(this, {"texture_displacementMap"}), dispTex);
            if (dispTex) uniforms.flags |= (1u << 24);
        }

        // Pack per-texture UV transforms into 3×2 affine matrices.
        packTransform(_baseColorTransform, uniforms.baseColorTransform0, uniforms.baseColorTransform1);
        packTransform(_normalTransform, uniforms.normalTransform0, uniforms.normalTransform1);
        packTransform(_metalRoughTransform, uniforms.metalRoughTransform0, uniforms.metalRoughTransform1);
        packTransform(_occlusionTransform, uniforms.occlusionTransform0, uniforms.occlusionTransform1);
        packTransform(_emissiveTransform, uniforms.emissiveTransform0, uniforms.emissiveTransform1);
    }

    void Material::getTextureSlots(std::vector<TextureSlot>& slots) const
    {
        slots.clear();

        // Resolve textures from typed properties, with parameter overrides.
        Texture* baseColorTex = _baseColorTexture;
        readTexture(getParam(this, {"texture_baseColorMap", "texture_diffuseMap", "baseColorTexture"}), baseColorTex);
        if (baseColorTex) slots.push_back({0, baseColorTex});

        Texture* normalTex = _normalTexture;
        readTexture(getParam(this, {"texture_normalMap", "normalTexture"}), normalTex);
        if (normalTex) slots.push_back({1, normalTex});

        Texture* mrTex = _metallicRoughnessTexture;
        readTexture(getParam(this, {"texture_metallicRoughnessMap", "metallicRoughnessTexture"}), mrTex);
        if (mrTex) slots.push_back({3, mrTex});

        Texture* occlusionTex = _occlusionTexture;
        readTexture(getParam(this, {"texture_occlusionMap", "occlusionTexture"}), occlusionTex);
        if (occlusionTex) slots.push_back({4, occlusionTex});

        Texture* emissiveTex = _emissiveTexture;
        readTexture(getParam(this, {"texture_emissiveMap", "emissiveTexture"}), emissiveTex);
        if (emissiveTex) slots.push_back({5, emissiveTex});
    }

    uint64_t Material::sortKey() const
    {
        const auto blendKey = static_cast<uint64_t>(_blendState ? _blendState->key() : 0);
        const auto depthKey = static_cast<uint64_t>(_depthState ? _depthState->key() : 0);
        const auto alphaModeKey = static_cast<uint64_t>(_alphaMode);
        const auto baseTextureBit = _hasBaseColorTexture ? 1ull : 0ull;
        const auto normalTextureBit = _hasNormalTexture ? 1ull : 0ull;
        const auto mrTextureBit = _hasMetallicRoughnessTexture ? 1ull : 0ull;
        const auto occlusionTextureBit = _hasOcclusionTexture ? 1ull : 0ull;
        const auto emissiveTextureBit = _hasEmissiveTexture ? 1ull : 0ull;
        const auto skyboxBit = _isSkybox ? 1ull : 0ull;
        return (_shaderVariantKey << 32) ^ (blendKey << 16) ^ (depthKey << 4) ^ (alphaModeKey << 3) ^
            (skyboxBit << 5) ^ (emissiveTextureBit << 4) ^ (occlusionTextureBit << 3) ^
            (mrTextureBit << 2) ^ (normalTextureBit << 1) ^ baseTextureBit;
    }

    void setDefaultMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<Material>& material) {
        assert(material != nullptr && "Cannot set null as default material");

        defaultMaterials[device.get()] = material;

        defaultMaterialDeviceCache.get<Material>(device,  [material] {
            return material;
        });
    }

    std::shared_ptr<Material> getDefaultMaterial(const std::shared_ptr<GraphicsDevice>& device)
    {
        const auto it = defaultMaterials.find(device.get());
        return it != defaultMaterials.end() ? it->second : nullptr;
    }
}
