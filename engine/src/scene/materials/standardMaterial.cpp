// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025
//

#include "standardMaterial.h"

#include <numbers>

#include <algorithm>
#include <cmath>

namespace visutwin::canvas
{
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
        _metalness = 1.0f;
        _useMetalness = false;
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
        _heightMapFactor = 1.0f;
        _heightMapBase = 0.5f;
        _heightMapShadow = 0.0f;
        _anisotropy = 0.0f;
        _anisotropyRotation = 0.0f;
        _useMetalnessSpecularColor = false;
        _specularityFactor = 1.0f;
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
        _clearCoatMapChannel = MapChannel::MAP_CHANNEL_G;
        _clearCoatGlossMapChannel = MapChannel::MAP_CHANNEL_G;

        _sheenColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        _sheenRoughness = 0.0f;
        _iridescenceIntensity = 0.0f;
        _iridescenceIOR = 1.3f;
        _iridescenceThicknessMax = 0.0f;
        _specGlossMap = nullptr;
        _detailNormalScale = 1.0f;
        _detailNormalMap = nullptr;
        _detailNormalTransform = TextureTransform();
        _displacementScale = 0.0f;
        _displacementBias = 0.5f;
        _displacementMap = nullptr;
        _useOrenNayar = false;

        _useFog = true;
        _useTonemap = true;
        _useLighting = true;
        _unlit = false;
        _vertexColorGamma = false;
        _useSkybox = true;
        _twoSidedLighting = false;

        setAlphaMode(AlphaMode::OPAQUE);
        setAlphaCutoff(0.0f);
        setOccludeDirect(false);
        setOccludeSpecular(SPECOCC_AO);
        setOccludeSpecularIntensity(1.0f);

    }

    namespace
    {
        /// An sRGB-authored colour channel in linear space.
        float toLinear(const float c)
        {
            return gammaToLinear(c);
        }
    }

    void StandardMaterial::updateUniforms(MaterialUniforms& uniforms) const
    {
        // Start with base Material implementation which reads typed properties + parameter overrides.
        // This handles the case where the GLB parser (or other code) sets properties on the base
        // Material rather than on StandardMaterial-specific members.
        Material::updateUniforms(uniforms);

        packMapTransforms(uniforms);
        packSurface(uniforms);
        packEmissiveAndAmbient(uniforms);
        packMsdf(uniforms);
        packAnisotropy(uniforms);
        packMetalnessSpecular(uniforms);
        packTransmission(uniforms);
        packLayers(uniforms);
        packFlags(uniforms);

        // Re-apply setParameter() overrides last: the typed writes above
        // (baseColor/metallic/roughness/normalScale/emissive) would otherwise
        // silently discard them, breaking the documented dual-binding contract.
        applyParameterOverrides(uniforms);
    }

    void StandardMaterial::packMapTransforms(MaterialUniforms& uniforms) const
    {
        // StandardMaterial's per-map UV transforms replace the base ones in the packed
        // block. They go into the block, never into the base fields: writing the material
        // from inside its own packer would re-dirty it on every pack.
        packTextureTransform({_diffuseMapTiling, _diffuseMapOffset, _diffuseMapRotation},
            uniforms.baseColorTransform0, uniforms.baseColorTransform1);
        packTextureTransform({_normalMapTiling, _normalMapOffset, _normalMapRotation},
            uniforms.normalTransform0, uniforms.normalTransform1);
        packTextureTransform({_metalnessMapTiling, _metalnessMapOffset, _metalnessMapRotation},
            uniforms.metalRoughTransform0, uniforms.metalRoughTransform1);
        packTextureTransform({_aoMapTiling, _aoMapOffset, _aoMapRotation},
            uniforms.occlusionTransform0, uniforms.occlusionTransform1);
        packTextureTransform({_emissiveMapTiling, _emissiveMapOffset, _emissiveMapRotation},
            uniforms.emissiveTransform0, uniforms.emissiveTransform1);
        if (_detailNormalMap) {
            packTextureTransform(_detailNormalTransform, uniforms.detailNormalTransform0,
                uniforms.detailNormalTransform1);
        }
    }

    void StandardMaterial::packSurface(MaterialUniforms& uniforms) const
    {
        // StandardMaterial's own scalars ALWAYS win, whatever textures are bound: a GLB
        // material binds its texture on the base Material, and a later setOpacity /
        // setMetalness / setGloss / setBumpiness must still apply. The parsers write these
        // scalars alongside the base-Material factors, so nothing loaded depends on
        // the base factors surviving.
        uniforms.baseColor[0] = _diffuse.r;
        uniforms.baseColor[1] = _diffuse.g;
        uniforms.baseColor[2] = _diffuse.b;
        uniforms.baseColor[3] = _opacity;

        // The specular workflow has no metalness: it is only read when
        // useMetalness is on, and a packed 0 gives both backends' metal-rough code
        // metallic = 0 without a branch of their own.
        uniforms.metallicFactor = _useMetalness ? _metalness : 0.0f;

        // Gloss convention: gloss=1 is smooth, roughness=0 is smooth.
        // If glossInvert, the value is already roughness.
        uniforms.roughnessFactor = _glossInvert ? _gloss : (1.0f - _gloss);

        uniforms.normalScale = _bumpiness;

        // Scalar maps modulate their factor by one channel of a texture. The gloss
        // factor travels separately, AUTHORED, because the map multiplies the factor
        // before glossInvert flips the product: under glossInvert a gloss map gives
        // roughness = factor x texel, not (1 - factor) x texel as gloss.
        uniforms.mapChannelParams[0] = _gloss;
        uniforms.glossMapParams[0] = _glossInvert ? 1.0f : 0.0f;
        uniforms.mapChannelParams[1] = _glossMap
            ? static_cast<float>(_glossMapChannel) : -1.0f;
        uniforms.mapChannelParams[2] = _thicknessMap
            ? static_cast<float>(_thicknessMapChannel) : -1.0f;
        uniforms.mapChannelParams[3] = _refractionMap
            ? static_cast<float>(_refractionMapChannel) : -1.0f;
    }

    void StandardMaterial::packEmissiveAndAmbient(MaterialUniforms& uniforms) const
    {
        // StandardMaterial always owns the emissive contribution: write _emissive * _emissiveIntensity
        // (linearized) directly, overriding whatever base Material::updateUniforms wrote from
        // _emissiveFactor. StandardMaterial.emissive is the authoritative emissive color, which
        // prevents common authoring glitches — e.g. specular-
        // glossiness GLB exporters that write emissiveFactor=(1,1,1) as a sentinel when no
        // emissive texture is present, which would otherwise produce fully-white glowing surfaces.
        // Users who want emission must call setEmissive()/setEmissiveIntensity(); when they do,
        // the linearize-first-then-scale order keeps HDR intensities (e.g. 200 × neon) in range
        // (pow(1, 2.2) * 200 = 200 linear, vs pow(200, 2.2) ≈ 1.7e5 which overflows fp16).
        uniforms.emissiveColor[0] = toLinear(_emissive.r) * _emissiveIntensity;
        uniforms.emissiveColor[1] = toLinear(_emissive.g) * _emissiveIntensity;
        uniforms.emissiveColor[2] = toLinear(_emissive.b) * _emissiveIntensity;
        uniforms.emissiveColor[3] = 1.0f;

        // Ambient tint, linearised as emissive is.
        uniforms.ambientTint[0] = toLinear(_ambient.r);
        uniforms.ambientTint[1] = toLinear(_ambient.g);
        uniforms.ambientTint[2] = toLinear(_ambient.b);
        uniforms.ambientTint[3] = 1.0f;
    }

    void StandardMaterial::packMsdf(MaterialUniforms& uniforms) const
    {
        // MSDF text: colours linear, alpha straight.
        if (!_msdfMap) {
            return;
        }
        uniforms.msdfParams[0] = _msdfPxRange;
        uniforms.msdfParams[1] = _msdfIntensity;
        uniforms.msdfParams[2] = static_cast<float>(std::max(_msdfMap->width(), 1u));
        uniforms.msdfParams[3] = static_cast<float>(std::max(_msdfMap->height(), 1u));
        uniforms.msdfOutlineColor[0] = toLinear(_msdfOutlineColor.r);
        uniforms.msdfOutlineColor[1] = toLinear(_msdfOutlineColor.g);
        uniforms.msdfOutlineColor[2] = toLinear(_msdfOutlineColor.b);
        uniforms.msdfOutlineColor[3] = _msdfOutlineColor.a;
        uniforms.msdfShadowColor[0] = toLinear(_msdfShadowColor.r);
        uniforms.msdfShadowColor[1] = toLinear(_msdfShadowColor.g);
        uniforms.msdfShadowColor[2] = toLinear(_msdfShadowColor.b);
        uniforms.msdfShadowColor[3] = _msdfShadowColor.a;
        uniforms.msdfOutlineShadow[0] = _msdfOutlineThickness;
        uniforms.msdfOutlineShadow[1] = _msdfShadowOffset.x;
        uniforms.msdfOutlineShadow[2] = _msdfShadowOffset.y;
        uniforms.msdfOutlineShadow[3] = 0.0f;
    }

    void StandardMaterial::packAnisotropy(MaterialUniforms& uniforms) const
    {
        // anisotropic specular. The strength goes up as a MAGNITUDE and the direction as
        // (cos, sin) of the rotation; the deprecated negative strength is rotation + 90.
        // Quarter turns are written exactly, so a material that only uses the deprecated
        // sign picks exactly the tangent or the bitangent.
        uniforms.anisotropy = std::abs(_anisotropy);

        double degrees = std::fmod(static_cast<double>(_anisotropyRotation) + (_anisotropy < 0.0f ? 90.0 : 0.0), 360.0);
        if (degrees < 0.0) {
            degrees += 360.0;
        }
        float c = 0.0f;
        float s = 0.0f;
        if (degrees == 0.0) {
            c = 1.0f;
        } else if (degrees == 90.0) {
            s = 1.0f;
        } else if (degrees == 180.0) {
            c = -1.0f;
        } else if (degrees == 270.0) {
            s = -1.0f;
        } else {
            const double radians = degrees * std::numbers::pi / 180.0;
            c = static_cast<float>(std::cos(radians));
            s = static_cast<float>(std::sin(radians));
        }
        uniforms.anisotropyParams[0] = c;
        uniforms.anisotropyParams[1] = s;
    }

    void StandardMaterial::packMetalnessSpecular(MaterialUniforms& uniforms) const
    {
        // Metalness workflow: the non-metal F0, from the
        // IOR, tinted by the specular colour when asked and scaled by the specularity
        // factor. In DOUBLE so the default IOR of 1.5 lands on exactly 0.04f, the
        // standard dielectric F0.
        const double ior = static_cast<double>(_refractionIndex);
        double f0 = (ior - 1.0) / (ior + 1.0);
        f0 *= f0;
        const auto linear = [](const float c) { return std::pow(std::max(static_cast<double>(c), 0.0), 2.2); };
        const double factor = static_cast<double>(_specularityFactor);
        const double r = _useMetalnessSpecularColor ? linear(_specular.r) : 1.0;
        const double g = _useMetalnessSpecularColor ? linear(_specular.g) : 1.0;
        const double b = _useMetalnessSpecularColor ? linear(_specular.b) : 1.0;
        uniforms.metalnessSpecular[0] = static_cast<float>(f0 * r * factor);
        uniforms.metalnessSpecular[1] = static_cast<float>(f0 * g * factor);
        uniforms.metalnessSpecular[2] = static_cast<float>(f0 * b * factor);
        uniforms.metalnessSpecular[3] = _specularityFactor;

        // The specular workflow's F0 and gloss. `specular` is authored in sRGB and
        // uploaded linear; gloss is the same
        // `gloss` the metalness workflow uses, with glossInvert applied.
        // (KHR_materials_pbrSpecularGlossiness.)
        uniforms.specGlossParams[0] = toLinear(_specular.r);
        uniforms.specGlossParams[1] = toLinear(_specular.g);
        uniforms.specGlossParams[2] = toLinear(_specular.b);
        uniforms.specGlossParams[3] = _glossInvert ? (1.0f - _gloss) : _gloss;
    }

    void StandardMaterial::packTransmission(MaterialUniforms& uniforms) const
    {
        uniforms.transmissionFactor = _transmissionFactor;
        uniforms.refractionIndex = _refractionIndex;
        uniforms.thickness = _thickness;
        uniforms.attenuationParams[0] = _attenuationColor.r;
        uniforms.attenuationParams[1] = _attenuationColor.g;
        uniforms.attenuationParams[2] = _attenuationColor.b;
        uniforms.attenuationParams[3] = _attenuationDistance;
        uniforms.dispersionParams[0] = _dispersion;

        // Decoupled dither strength. Negative means "unset", which the shaders read as
        // "fall back to opacity" — opacity then drives both the blend and the dither.
        uniforms.dispersionParams[1] = _alphaDither;
    }

    void StandardMaterial::packLayers(MaterialUniforms& uniforms) const
    {
        // parallax / height map.
        if (_heightMap) {
            uniforms.heightMapFactor = _heightMapFactor;
            uniforms.heightMapParams[0] = std::clamp(_heightMapBase, 0.0f, 1.0f);
            uniforms.heightMapParams[1] = std::clamp(_heightMapShadow, 0.0f, 1.0f);
        }

        // clearcoat.
        if (_clearCoat > 0.0f) {
            uniforms.clearCoatFactor = _clearCoat;
            const float ccGloss = _clearCoatGlossInvert ? (1.0f - _clearCoatGloss) : _clearCoatGloss;
            uniforms.clearCoatRoughness = 1.0f - ccGloss;
            // A clearcoat gloss map multiplies the AUTHORED factor and inverts after,
            // so it needs the factor and the flag rather than the packed roughness.
            uniforms.glossMapParams[1] = _clearCoatGlossInvert ? 1.0f : 0.0f;
            uniforms.glossMapParams[2] = _clearCoatGloss;
            uniforms.clearCoatBumpiness = _clearCoatBumpiness;
            uniforms.clearCoatMapChannels[0] = static_cast<float>(_clearCoatMapChannel);
            uniforms.clearCoatMapChannels[1] = static_cast<float>(_clearCoatGlossMapChannel);
        }

        // sheen (KHR_materials_sheen).
        uniforms.sheenColor[0] = _sheenColor.r;
        uniforms.sheenColor[1] = _sheenColor.g;
        uniforms.sheenColor[2] = _sheenColor.b;
        uniforms.sheenColor[3] = _sheenRoughness;

        // iridescence (KHR_materials_iridescence).
        uniforms.iridescenceParams[0] = _iridescenceIntensity;
        uniforms.iridescenceParams[1] = _iridescenceIOR;
        // z (minimum thickness) is unused: only a thickness map would interpolate towards
        // it, and there is no iridescence thickness map.
        uniforms.iridescenceParams[2] = 0.0f;
        uniforms.iridescenceParams[3] = _iridescenceThicknessMax;

        // detail normals + displacement.
        uniforms.detailDisplacementParams[0] = _detailNormalScale;
        uniforms.detailDisplacementParams[1] = _displacementScale;
        uniforms.detailDisplacementParams[2] = _displacementBias;
        uniforms.detailDisplacementParams[3] = 0.0f;
    }

    void StandardMaterial::packFlags(MaterialUniforms& uniforms) const
    {
        // Every bit StandardMaterial adds to the flags word the base Material packed.
        uint32_t& flags = uniforms.flags;

        // StandardMaterial adds twoSidedLighting support to the doubleSided flag.
        if (_twoSidedLighting) flags |= (1u << 3);    // bit 3: doubleSided

        // Texture flags for StandardMaterial-specific texture maps (if set).
        if (_diffuseMap)    flags |= 1u;              // bit 0: hasBaseColorMap
        if (_normalMap)     flags |= (1u << 2);       // bit 2: hasNormalMap
        if (_metalnessMap)  flags |= (1u << 6);       // bit 6: hasMetallicRoughnessMap
        if (_aoMap)         flags |= (1u << 9);       // bit 9: hasOcclusionMap
        if (_emissiveMap)   flags |= (1u << 11);      // bit 11: hasEmissiveMap
        if (_clearCoat > 0.0f) {
            if (_clearCoatMap)       flags |= (1u << 14);  // bit 14: hasClearCoatMap
            if (_clearCoatGlossMap)  flags |= (1u << 15);  // bit 15: hasClearCoatGlossMap
            if (_clearCoatNormalMap) flags |= (1u << 16);  // bit 16: hasClearCoatNormalMap
        }
        if (_heightMap)     flags |= (1u << 17);      // bit 17: hasHeightMap
        // bit 18: useSkybox OFF. Stored inverted so a zero flags word keeps the scene
        // environment, the default. Turning it off drops the
        // environment atlas for this material; SH probes and the flat ambient remain.
        if (!_useSkybox)    flags |= (1u << 18);
        // bit 19: hasOpacityMap (slot 34; a separate image at set-1 binding 34 on Vulkan).
        // Bit 20 is the only free flag bit.
        if (_opacityMap)    flags |= (1u << 19);
        if (_specGlossMap)  flags |= (1u << 21);      // bit 21: hasSpecGlossMap
        if (_detailNormalMap) flags |= (1u << 22);    // bit 22: hasDetailNormalMap
        if (_displacementMap) flags |= (1u << 24);    // bit 24: hasDisplacementMap

        // Vertex color routing.
        // Bit 28 is the DISABLE for the diffuse lane so that a zero flags word keeps
        // the long-standing "vertex colors tint diffuse" behaviour.
        if (_emissiveVertexColor)  flags |= (1u << 23);
        if (!_diffuseVertexColor)  flags |= (1u << 28);

        // bits 25-27: opacity dither matrix (DitherMode). Which matrix is a runtime value rather
        // than a shader variant, so changing it costs no recompile — VT_FEATURE_OPACITY_DITHER
        // only gates whether the dither block exists at all.
        flags |= (static_cast<uint32_t>(_opacityDitherMode) & 0x7u) << 25;

        // bits 29-31: shadow-pass dither matrix, kept independent of the forward one so a
        // caster can dither its shadow without dithering itself (and the reverse).
        flags |= (static_cast<uint32_t>(_opacityShadowDitherMode) & 0x7u) << 29;
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
        overrideSlot(kLightMapTextureSlot, _lightMap);
        // Spec-gloss reuses the metal-rough binding (slot 3) — the two
        // parameterizations are mutually exclusive and VT_FEATURE_SPEC_GLOSS
        // reinterprets the sample as rgb=specular color (sRGB), a=glossiness.
        overrideSlot(3, _specGlossMap);
        // Detail normal overlay at slot 23.
        overrideSlot(23, _detailNormalMap);
        // Scalar maps. Slots 0-30 are all taken, so these extend the range.
        overrideSlot(31, _glossMap);
        overrideSlot(32, _thicknessMap);
        overrideSlot(33, _refractionMap);
        // Opacity map, alpha channel by default. Metal only.
        overrideSlot(34, _opacityMap);
        // Vertex displacement map: routed to VERTEX texture slot 0 via the
        // >= 100 sentinel (see MetalTextureBinder::bindMaterialTextures).
        overrideSlot(100, _displacementMap);
    }

    std::shared_ptr<Material> StandardMaterial::clone() const
    {
        auto copy = std::make_shared<StandardMaterial>(*this);
        copy->detachSharedState();
        return copy;
    }
}
