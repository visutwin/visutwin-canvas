// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//

#include "standardMaterial.h"

#include <algorithm>
#include <cmath>

namespace visutwin::canvas
{
    namespace
    {
        constexpr float DEG_TO_RAD_SM = 3.14159265358979323846f / 180.0f;

        void packTransformSM(const TextureTransform& t, float row0[4], float row1[4])
        {
            const float cr = std::cos(t.rotation * DEG_TO_RAD_SM);
            const float sr = std::sin(t.rotation * DEG_TO_RAD_SM);
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
    StandardMaterial::StandardMaterial()
    {
        reset();
    }

    void StandardMaterial::reset()
    {
        // StandardMaterial defaults.
        setTransparent(false);
        setCullMode(CullMode::CULLFACE_BACK);

        _diffuse = Color(1.0f, 1.0f, 1.0f, 1.0f);
        _diffuseMap = nullptr;
        _specular = Color(0.0f, 0.0f, 0.0f, 1.0f);
        _metalness = 0.0f;
        _useMetalness = true;
        _metalnessMap = nullptr;
        _gloss = 0.25f;
        _glossInvert = false;
        _glossMap = nullptr;
        _emissive = Color(0.0f, 0.0f, 0.0f, 1.0f);
        _emissiveIntensity = 1.0f;
        _emissiveMap = nullptr;
        _normalMap = nullptr;
        _bumpiness = 1.0f;
        _heightMap = nullptr;
        _heightMapFactor = 0.05f;
        _anisotropy = 0.0f;
        _transmissionFactor = 0.0f;
        _refractionIndex = 1.5f;
        _thickness = 0.0f;
        _opacity = 1.0f;
        _opacityMap = nullptr;
        _aoMap = nullptr;

        _diffuseMapTiling = Vector2(1.0f, 1.0f);
        _diffuseMapOffset = Vector2(0.0f, 0.0f);
        _diffuseMapRotation = 0.0f;
        _normalMapTiling = Vector2(1.0f, 1.0f);
        _normalMapOffset = Vector2(0.0f, 0.0f);
        _normalMapRotation = 0.0f;
        _metalnessMapTiling = Vector2(1.0f, 1.0f);
        _metalnessMapOffset = Vector2(0.0f, 0.0f);
        _metalnessMapRotation = 0.0f;
        _aoMapTiling = Vector2(1.0f, 1.0f);
        _aoMapOffset = Vector2(0.0f, 0.0f);
        _aoMapRotation = 0.0f;
        _emissiveMapTiling = Vector2(1.0f, 1.0f);
        _emissiveMapOffset = Vector2(0.0f, 0.0f);
        _emissiveMapRotation = 0.0f;

        _reflectionMap = nullptr;
        _clearCoat = 0.0f;
        _clearCoatGloss = 1.0f;
        _clearCoatGlossInvert = false;
        _clearCoatBumpiness = 1.0f;
        _clearCoatMap = nullptr;
        _clearCoatGlossMap = nullptr;
        _clearCoatNormalMap = nullptr;

        _sheenColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        _sheenRoughness = 0.0f;
        _sheenMap = nullptr;
        _iridescenceIntensity = 0.0f;
        _iridescenceIOR = 1.3f;
        _iridescenceThicknessMin = 100.0f;
        _iridescenceThicknessMax = 400.0f;
        _iridescenceMap = nullptr;
        _iridescenceThicknessMap = nullptr;
        _specularColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
        _glossiness = 1.0f;
        _specGlossMap = nullptr;
        _detailNormalScale = 1.0f;
        _detailNormalMap = nullptr;
        _detailNormalTransform = TextureTransform();
        _displacementScale = 0.0f;
        _displacementBias = 0.5f;
        _displacementMap = nullptr;
        _useOrenNayar = false;

        _useFog = true;
        _useLighting = true;
        _useSkybox = true;
        _twoSidedLighting = false;

        setAlphaMode(AlphaMode::OPAQUE);
        setAlphaCutoff(0.0f);
        setOccludeDirect(false);
        setOccludeSpecular(SPECOCC_AO);
        setOccludeSpecularIntensity(1.0f);

        _dirtyShader = true;
    }

    void StandardMaterial::updateUniforms(MaterialUniforms& uniforms) const
    {
        // Push StandardMaterial per-map transforms into base Material transform fields
        // so that Material::updateUniforms() packs them into the GPU struct.
        auto* self = const_cast<StandardMaterial*>(this);
        self->setBaseColorTransform({_diffuseMapTiling, _diffuseMapOffset, _diffuseMapRotation});
        self->setNormalTransform({_normalMapTiling, _normalMapOffset, _normalMapRotation});
        self->setMetalRoughTransform({_metalnessMapTiling, _metalnessMapOffset, _metalnessMapRotation});
        self->setOcclusionTransform({_aoMapTiling, _aoMapOffset, _aoMapRotation});
        self->setEmissiveTransform({_emissiveMapTiling, _emissiveMapOffset, _emissiveMapRotation});

        // Start with base Material implementation which reads typed properties + parameter overrides.
        // This handles the case where the GLB parser (or other code) sets properties on the base
        // Material rather than on StandardMaterial-specific members.
        Material::updateUniforms(uniforms);

        // Apply StandardMaterial-specific overrides on top when StandardMaterial properties
        // have been explicitly set (i.e. when using the StandardMaterial API directly).
        if (_diffuseMap || !baseColorTexture()) {
            // StandardMaterial owns the diffuse color; override base color.
            uniforms.baseColor[0] = _diffuse.r;
            uniforms.baseColor[1] = _diffuse.g;
            uniforms.baseColor[2] = _diffuse.b;
            uniforms.baseColor[3] = _opacity;

            uniforms.metallicFactor = _metalness;

            // Gloss convention: gloss=1 is smooth, roughness=0 is smooth.
            // If glossInvert, the value is already roughness.
            uniforms.roughnessFactor = _glossInvert ? _gloss : (1.0f - _gloss);

            uniforms.normalScale = _bumpiness;
        }

        // StandardMaterial always owns the emissive contribution: write _emissive * _emissiveIntensity
        // (linearized) directly, overriding whatever base Material::updateUniforms wrote from
        // _emissiveFactor. This matches PlayCanvas semantics (StandardMaterial.emissive is the
        // authoritative emissive color) and prevents common authoring glitches — e.g. specular-
        // glossiness GLB exporters that write emissiveFactor=(1,1,1) as a sentinel when no
        // emissive texture is present, which would otherwise produce fully-white glowing surfaces.
        // Users who want emission must call setEmissive()/setEmissiveIntensity(); when they do,
        // the linearize-first-then-scale order keeps HDR intensities (e.g. 200 × neon) in range
        // (pow(1, 2.2) * 200 = 200 linear, vs pow(200, 2.2) ≈ 1.7e5 which overflows fp16).
        uniforms.emissiveColor[0] = std::pow(std::max(_emissive.r, 0.0f), 2.2f) * _emissiveIntensity;
        uniforms.emissiveColor[1] = std::pow(std::max(_emissive.g, 0.0f), 2.2f) * _emissiveIntensity;
        uniforms.emissiveColor[2] = std::pow(std::max(_emissive.b, 0.0f), 2.2f) * _emissiveIntensity;
        uniforms.emissiveColor[3] = 1.0f;

        // StandardMaterial adds twoSidedLighting support to the doubleSided flag.
        if (_twoSidedLighting) {
            uniforms.flags |= (1u << 3);    // bit 3: doubleSided
        }

        // Override texture flags with StandardMaterial-specific texture maps (if set).
        if (_diffuseMap)    uniforms.flags |= 1u;              // bit 0: hasBaseColorMap
        if (_normalMap)     uniforms.flags |= (1u << 2);       // bit 2: hasNormalMap
        if (_metalnessMap)  uniforms.flags |= (1u << 6);       // bit 6: hasMetallicRoughnessMap
        if (_aoMap)         uniforms.flags |= (1u << 9);       // bit 9: hasOcclusionMap
        if (_emissiveMap)   uniforms.flags |= (1u << 11);      // bit 11: hasEmissiveMap

        // anisotropic specular.
        uniforms.anisotropy = _anisotropy;

        // transmission / refraction.
        uniforms.transmissionFactor = _transmissionFactor;
        uniforms.refractionIndex = _refractionIndex;
        uniforms.thickness = _thickness;

        // parallax / height map uniform packing.
        if (_heightMap) {
            uniforms.heightMapFactor = _heightMapFactor;
            uniforms.flags |= (1u << 17);   // bit 17: hasHeightMap
        }

        // clearcoat uniform packing.
        if (_clearCoat > 0.0f) {
            uniforms.clearCoatFactor = _clearCoat;
            const float ccGloss = _clearCoatGlossInvert ? (1.0f - _clearCoatGloss) : _clearCoatGloss;
            uniforms.clearCoatRoughness = 1.0f - ccGloss;
            uniforms.clearCoatBumpiness = _clearCoatBumpiness;
            if (_clearCoatMap)       uniforms.flags |= (1u << 14);  // bit 14: hasClearCoatMap
            if (_clearCoatGlossMap)  uniforms.flags |= (1u << 15);  // bit 15: hasClearCoatGlossMap
            if (_clearCoatNormalMap) uniforms.flags |= (1u << 16);  // bit 16: hasClearCoatNormalMap
        }

        // sheen uniform packing (KHR_materials_sheen).
        uniforms.sheenColor[0] = _sheenColor.r;
        uniforms.sheenColor[1] = _sheenColor.g;
        uniforms.sheenColor[2] = _sheenColor.b;
        uniforms.sheenColor[3] = _sheenRoughness;
        if (_sheenMap) uniforms.flags |= (1u << 18);  // bit 18: hasSheenMap

        // iridescence uniform packing (KHR_materials_iridescence).
        uniforms.iridescenceParams[0] = _iridescenceIntensity;
        uniforms.iridescenceParams[1] = _iridescenceIOR;
        uniforms.iridescenceParams[2] = _iridescenceThicknessMin;
        uniforms.iridescenceParams[3] = _iridescenceThicknessMax;
        if (_iridescenceMap)          uniforms.flags |= (1u << 19);  // bit 19: hasIridescenceMap
        if (_iridescenceThicknessMap) uniforms.flags |= (1u << 20);  // bit 20: hasIridescenceThicknessMap

        // spec-gloss uniform packing (KHR_materials_pbrSpecularGlossiness).
        uniforms.specGlossParams[0] = _specularColor.r;
        uniforms.specGlossParams[1] = _specularColor.g;
        uniforms.specGlossParams[2] = _specularColor.b;
        uniforms.specGlossParams[3] = _glossiness;
        if (_specGlossMap) uniforms.flags |= (1u << 21);  // bit 21: hasSpecGlossMap

        // detail normals + displacement uniform packing.
        uniforms.detailDisplacementParams[0] = _detailNormalScale;
        uniforms.detailDisplacementParams[1] = _displacementScale;
        uniforms.detailDisplacementParams[2] = _displacementBias;
        uniforms.detailDisplacementParams[3] = 0.0f;
        if (_detailNormalMap) {
            uniforms.flags |= (1u << 22);  // bit 22: hasDetailNormalMap
            packTransformSM(_detailNormalTransform, uniforms.detailNormalTransform0, uniforms.detailNormalTransform1);
        }
        if (_displacementMap)  uniforms.flags |= (1u << 24);  // bit 24: hasDisplacementMap
    }

    void StandardMaterial::getTextureSlots(std::vector<TextureSlot>& slots) const
    {
        // Start with base Material implementation (picks up textures set via base API).
        Material::getTextureSlots(slots);

        // Override with StandardMaterial-specific textures where set.
        auto overrideSlot = [&](int slotIndex, Texture* texture) {
            if (!texture) {
                return;
            }
            for (auto& [slot, tex] : slots) {
                if (slot == slotIndex) {
                    tex = texture;
                    return;
                }
            }
            slots.push_back({slotIndex, texture});
        };
        overrideSlot(0, _diffuseMap);
        overrideSlot(1, _normalMap);
        overrideSlot(3, _metalnessMap);
        overrideSlot(4, _aoMap);
        overrideSlot(5, _emissiveMap);
        overrideSlot(7, _clearCoatMap);
        overrideSlot(9, _reflectionMap);
        overrideSlot(13, _clearCoatGlossMap);
        overrideSlot(14, _clearCoatNormalMap);
        overrideSlot(17, _heightMap);
        overrideSlot(18, _sheenMap);
        overrideSlot(19, _iridescenceMap);
        overrideSlot(20, _iridescenceThicknessMap);
        overrideSlot(21, _specGlossMap);
        overrideSlot(22, _detailNormalMap);
        overrideSlot(23, _displacementMap);
    }
}
