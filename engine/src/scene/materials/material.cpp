// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025
//
#include <cstring>

#include <spdlog/spdlog.h>

#include "material.h"
#include "materialParameterRead.h"

#include <atomic>

#include "scene/shader-lib/shaderChunks.h"

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
    uint32_t Material::_nextId = 0;

    DeviceCache defaultMaterialDeviceCache;
    std::unordered_map<GraphicsDevice*, std::shared_ptr<Material>> defaultMaterials;


    // Pre-computes the 3x2 affine matrix from tiling, offset and rotation.
    void Material::packTextureTransform(const TextureTransform& t, float row0[4], float row1[4])
    {
        constexpr float degToRad = 3.14159265358979323846f / 180.0f;
        const float cr = std::cos(t.rotation * degToRad);
        const float sr = std::sin(t.rotation * degToRad);
        row0[0] = cr * t.tiling.x;
        row0[1] = -sr * t.tiling.y;
        row0[2] = t.offset.x;
        row0[3] = 0.0f;
        row1[0] = sr * t.tiling.x;
        row1[1] = cr * t.tiling.y;
        row1[2] = 1.0f - t.tiling.y - t.offset.y;
        row1[3] = 0.0f;
    }

    uint64_t Material::nextUniformsVersion()
    {
        static std::atomic<uint64_t> counter{0};
        return ++counter;
    }

    Material::Material()
    {
        _blendState = std::make_shared<BlendState>();
        _depthState = std::make_shared<DepthState>();
    }

    void Material::setAlphaMode(const AlphaMode mode)
    {
        markUniformsDirty();
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
        markUniformsDirty();
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
        markUniformsDirty();
        _parameters.clear();
    }

    const Material::ParameterValue* Material::parameter(const std::string& name) const
    {
        const auto it = _parameters.find(name);
        return it == _parameters.end() ? nullptr : &it->second;
    }

    const MaterialUniforms& Material::packedUniforms() const
    {
        if (_uniformsDirty) {
            packAll(_cachedUniforms);
            // Cleared after packing, so a pack that somehow dirtied the material would
            // leave it dirty. Packers must not write to their material: StandardMaterial's
            // UV transforms go straight into the block.
            _uniformsDirty = false;
        }

#ifndef NDEBUG
        // Debug builds additionally re-pack and compare, so a mutator that forgets to
        // call markUniformsDirty() is reported here instead of showing up later as a
        // surface that ignores an edit. The CACHED value is still what gets returned,
        // so debug and release behave identically and a test can catch the staleness.
        MaterialUniforms fresh{};
        packAll(fresh);
        _uniformsDirty = false;
        if (std::memcmp(&fresh, &_cachedUniforms, sizeof(MaterialUniforms)) != 0) {
            spdlog::error("Material '{}': packed uniform cache is stale — a mutator is "
                          "missing markUniformsDirty()", name());
        }
#endif
        return _cachedUniforms;
    }

    void Material::packAll(MaterialUniforms& uniforms) const
    {
        updateUniforms(uniforms);
        if (_pickPass) {
            uniforms.baseColor[0] = _pickColor.r;
            uniforms.baseColor[1] = _pickColor.g;
            uniforms.baseColor[2] = _pickColor.b;
        }
    }

    void Material::applyParameterOverrides(MaterialUniforms& uniforms) const
    {
        // Nine lookups of up to two names each, and unordered_map::find hashes the
        // string before it can discover the map is empty — which it is for every
        // material that never calls setParameter, i.e. nearly all of them.
        if (_parameters.empty()) {
            return;
        }

        readParameterColor4(findMaterialParameter(this, {"material_baseColor", "baseColorFactor"}), uniforms.baseColor);
        {
            // Parameter override convention is sRGB input — linearize to match the typed path.
            float emissiveOverride[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (readParameterColor4(findMaterialParameter(this, {"material_emissive", "emissiveFactor"}), emissiveOverride)) {
                uniforms.emissiveColor[0] = gammaToLinear(emissiveOverride[0]);
                uniforms.emissiveColor[1] = gammaToLinear(emissiveOverride[1]);
                uniforms.emissiveColor[2] = gammaToLinear(emissiveOverride[2]);
                uniforms.emissiveColor[3] = emissiveOverride[3];
            }
        }
        readParameterFloat(findMaterialParameter(this, {"material_alphaCutoff", "alphaCutoff"}), uniforms.alphaCutoff);
        readParameterFloat(findMaterialParameter(this, {"material_metallic", "metallicFactor"}), uniforms.metallicFactor);
        readParameterFloat(findMaterialParameter(this, {"material_roughness", "roughnessFactor"}), uniforms.roughnessFactor);
        readParameterFloat(findMaterialParameter(this, {"material_normalScale", "normalScale"}), uniforms.normalScale);
        readParameterFloat(findMaterialParameter(this, {"material_occlusionStrength", "occlusionStrength"}), uniforms.occlusionStrength);
        readParameterFloat(findMaterialParameter(this, {"material_occludeSpecularIntensity", "occludeSpecularIntensity"}),
            uniforms.occludeSpecularIntensity);
        {
            int occludeSpecularMode = static_cast<int>(uniforms.occludeSpecularMode);
            readParameterInt(findMaterialParameter(this, {"material_occludeSpecular", "occludeSpecular"}), occludeSpecularMode);
            occludeSpecularMode = std::clamp(occludeSpecularMode,
                static_cast<int>(SPECOCC_NONE), static_cast<int>(SPECOCC_GLOSSDEPENDENT));
            uniforms.occludeSpecularMode = static_cast<uint32_t>(occludeSpecularMode);
        }
    }

    void Material::updateUniforms(MaterialUniforms& uniforms) const
    {
        // Pack typed properties into GPU struct.
        uniforms.baseColor[0] = _baseColorFactor.r;
        uniforms.baseColor[1] = _baseColorFactor.g;
        uniforms.baseColor[2] = _baseColorFactor.b;
        uniforms.baseColor[3] = _baseColorFactor.a;
        // Emissive is authored in sRGB (hence the .gamma() conversion the glTF parser applies
        // to glTF's linear emissiveFactor). The GPU
        // wants linear HDR, and a StandardMaterial subclass may multiply by emissiveIntensity > 1.
        // Linearize FIRST here so the intensity scaling (applied by StandardMaterial::updateUniforms
        // below) happens in linear space — applying pow() to intensity-scaled sRGB blows up to
        // +Inf for bright neon (e.g. 200 * 1.0 → pow(200, 2.2) ≈ 1.7e5, overflowing fp16 targets).
        uniforms.emissiveColor[0] = gammaToLinear(_emissiveFactor.r);
        uniforms.emissiveColor[1] = gammaToLinear(_emissiveFactor.g);
        uniforms.emissiveColor[2] = gammaToLinear(_emissiveFactor.b);
        uniforms.emissiveColor[3] = _emissiveFactor.a;
        uniforms.alphaCutoff = _alphaCutoff;
        uniforms.metallicFactor = _metallicFactor;
        uniforms.roughnessFactor = _roughnessFactor;
        uniforms.normalScale = _normalScale;
        uniforms.occlusionStrength = _occlusionStrength;
        uniforms.occludeSpecularMode = _occludeSpecular;
        uniforms.occludeSpecularIntensity = _occludeSpecularIntensity;
        uniforms.flags = 0u;

        // Allow custom parameter overrides (`material_*` and short alias names alike).
        applyParameterOverrides(uniforms);

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
        readParameterInt(findMaterialParameter(this, {"baseColorUvSet"}), baseUvSet);
        readParameterInt(findMaterialParameter(this, {"normalUvSet"}), normalUvSet);
        readParameterInt(findMaterialParameter(this, {"metallicRoughnessUvSet"}), metallicUvSet);
        readParameterInt(findMaterialParameter(this, {"occlusionUvSet"}), occlusionUvSet);
        readParameterInt(findMaterialParameter(this, {"emissiveUvSet"}), emissiveUvSet);
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
        readParameterInt(findMaterialParameter(this, {"material_occludeDirect", "occludeDirect"}), occludeDirect);
        if (occludeDirect != 0) uniforms.flags |= (1u << 13);

        // Height/parallax map: flag bit 17.
        Texture* heightTex = nullptr;
        readParameterTexture(findMaterialParameter(this, {"texture_heightMap"}), heightTex);
        if (heightTex) uniforms.flags |= (1u << 17);
        readParameterFloat(findMaterialParameter(this, {"material_heightMapFactor", "heightMapFactor"}), uniforms.heightMapFactor);

        // Anisotropy: parameter override.
        readParameterFloat(findMaterialParameter(this, {"material_anisotropy", "anisotropy"}), uniforms.anisotropy);

        // Transmission/refraction: parameter overrides.
        readParameterFloat(findMaterialParameter(this, {"material_transmissionFactor", "transmissionFactor"}), uniforms.transmissionFactor);
        readParameterFloat(findMaterialParameter(this, {"material_refractionIndex", "refractionIndex"}), uniforms.refractionIndex);
        readParameterFloat(findMaterialParameter(this, {"material_thickness", "thickness"}), uniforms.thickness);

        // Sheen: parameter overrides (KHR_materials_sheen).
        readParameterColor4(findMaterialParameter(this, {"material_sheenColor", "sheenColor"}), uniforms.sheenColor);
        readParameterFloat(findMaterialParameter(this, {"material_sheenRoughness", "sheenRoughness"}), uniforms.sheenColor[3]);

        // Iridescence: parameter overrides (KHR_materials_iridescence).
        readParameterFloat(findMaterialParameter(this, {"material_iridescenceIntensity", "iridescenceIntensity"}), uniforms.iridescenceParams[0]);
        readParameterFloat(findMaterialParameter(this, {"material_iridescenceIOR", "iridescenceIOR"}), uniforms.iridescenceParams[1]);
        readParameterFloat(findMaterialParameter(this, {"material_iridescenceThicknessMax", "iridescenceThicknessMax"}), uniforms.iridescenceParams[3]);
        // No sheen or iridescence map parameters: neither backend samples such maps, and
        // flag bits 18-20 mean skybox-off and hasOpacityMap on a StandardMaterial,
        // so a stray texture parameter must not be able to set them.

        // Spec-Gloss: parameter overrides (KHR_materials_pbrSpecularGlossiness).
        readParameterColor4(findMaterialParameter(this, {"material_specularColor", "specularColor"}), uniforms.specGlossParams);
        readParameterFloat(findMaterialParameter(this, {"material_glossiness", "glossiness"}), uniforms.specGlossParams[3]);
        {
            Texture* sgTex = nullptr;
            readParameterTexture(findMaterialParameter(this, {"texture_specGlossMap"}), sgTex);
            if (sgTex) uniforms.flags |= (1u << 21);
        }

        // Detail normals: parameter overrides.
        readParameterFloat(findMaterialParameter(this, {"material_detailNormalScale", "detailNormalScale"}), uniforms.detailDisplacementParams[0]);
        {
            Texture* detailTex = nullptr;
            readParameterTexture(findMaterialParameter(this, {"texture_detailNormalMap"}), detailTex);
            if (detailTex) uniforms.flags |= (1u << 22);
        }

        // Displacement: parameter overrides.
        readParameterFloat(findMaterialParameter(this, {"material_displacementScale", "displacementScale"}), uniforms.detailDisplacementParams[1]);
        readParameterFloat(findMaterialParameter(this, {"material_displacementBias", "displacementBias"}), uniforms.detailDisplacementParams[2]);
        {
            Texture* dispTex = nullptr;
            readParameterTexture(findMaterialParameter(this, {"texture_displacementMap"}), dispTex);
            if (dispTex) uniforms.flags |= (1u << 24);
        }

        // Pack per-texture UV transforms into 3×2 affine matrices.
        packTextureTransform(_baseColorTransform, uniforms.baseColorTransform0, uniforms.baseColorTransform1);
        packTextureTransform(_normalTransform, uniforms.normalTransform0, uniforms.normalTransform1);
        packTextureTransform(_metalRoughTransform, uniforms.metalRoughTransform0, uniforms.metalRoughTransform1);
        packTextureTransform(_occlusionTransform, uniforms.occlusionTransform0, uniforms.occlusionTransform1);
        packTextureTransform(_emissiveTransform, uniforms.emissiveTransform0, uniforms.emissiveTransform1);
    }

    void Material::getTextureSlots(std::vector<TextureSlot>& slots) const
    {
        slots.clear();

        // Resolve textures from typed properties, with parameter overrides.
        Texture* baseColorTex = _baseColorTexture;
        readParameterTexture(findMaterialParameter(this, {"texture_baseColorMap", "texture_diffuseMap", "baseColorTexture"}), baseColorTex);
        if (baseColorTex) slots.push_back({0, baseColorTex});

        Texture* normalTex = _normalTexture;
        readParameterTexture(findMaterialParameter(this, {"texture_normalMap", "normalTexture"}), normalTex);
        if (normalTex) slots.push_back({1, normalTex});

        Texture* mrTex = _metallicRoughnessTexture;
        readParameterTexture(findMaterialParameter(this, {"texture_metallicRoughnessMap", "metallicRoughnessTexture"}), mrTex);
        if (mrTex) slots.push_back({3, mrTex});

        Texture* occlusionTex = _occlusionTexture;
        readParameterTexture(findMaterialParameter(this, {"texture_occlusionMap", "occlusionTexture"}), occlusionTex);
        if (occlusionTex) slots.push_back({4, occlusionTex});

        Texture* emissiveTex = _emissiveTexture;
        readParameterTexture(findMaterialParameter(this, {"texture_emissiveMap", "emissiveTexture"}), emissiveTex);
        if (emissiveTex) slots.push_back({5, emissiveTex});
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

    void Material::setShaderChunk(const std::string& name, std::string source)
    {
        markUniformsDirty();
        _shaderChunkOverrides[name] = std::move(source);
        _shaderChunksHash = ShaderChunks::hashChunkMap(_shaderChunkOverrides);
    }

    bool Material::removeShaderChunk(const std::string& name)
    {
        const bool removed = _shaderChunkOverrides.erase(name) > 0;
        if (removed) {
            _shaderChunksHash = ShaderChunks::hashChunkMap(_shaderChunkOverrides);
        }
        return removed;
    }

    void Material::clearShaderChunks()
    {
        markUniformsDirty();
        if (!_shaderChunkOverrides.empty()) {
            _shaderChunkOverrides.clear();
            _shaderChunksHash = 0;
        }
    }

    void Material::detachSharedState()
    {
        // The copy constructor shares the state objects with the original; duplicate them
        // so the clone can be reconfigured on its own.
        if (_blendState) {
            _blendState = std::make_shared<BlendState>(*_blendState);
        }
        if (_depthState) {
            _depthState = std::make_shared<DepthState>(*_depthState);
        }
    }

    std::shared_ptr<Material> Material::clone() const
    {
        auto copy = std::make_shared<Material>(*this);
        copy->detachSharedState();
        return copy;
    }
}
