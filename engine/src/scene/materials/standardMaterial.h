// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#pragma once
#include <algorithm>

#include "material.h"

namespace visutwin::canvas
{
    class Texture;

    /**
     * @brief Full PBR material with metalness/roughness workflow and advanced surface features.
     * @ingroup group_scene_materials
     *
     * StandardMaterial is the main, general-purpose material for physically-based rendering.
     * It supports diffuse, specular, metalness, gloss/roughness, emissive, normal, AO, and
     * height maps. Advanced features include clearcoat, anisotropy, sheen, iridescence,
     * transmission, and displacement. Each map input is multiplied with its constant value
     * and optional vertex colors.
     */
    class StandardMaterial : public Material
    {
    public:
        StandardMaterial();

        void reset();

        std::shared_ptr<Material> clone() const override;

        void updateUniforms(MaterialUniforms& uniforms) const override;
        void getTextureSlots(std::vector<TextureSlot>& slots) const override;

        // --- Diffuse ---
        const Color& diffuse() const { return _diffuse; }
        void setDiffuse(const Color& value) { _diffuse = value; markUniformsDirty(); }
        Texture* diffuseMap() const { return _diffuseMap; }
        void setDiffuseMap(Texture* texture) { _diffuseMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Specular ---
        const Color& specular() const { return _specular; }
        void setSpecular(const Color& value) { _specular = value; markUniformsDirty(); }
        // --- Metalness ---
        float metalness() const { return _metalness; }
        void setMetalness(const float value) { _metalness = value; markUniformsDirty(); }
        bool useMetalness() const { return _useMetalness; }
        void setUseMetalness(const bool value) { _useMetalness = value; _dirtyShader = true; markUniformsDirty(); }
        Texture* metalnessMap() const { return _metalnessMap; }
        void setMetalnessMap(Texture* texture) { _metalnessMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Gloss / Roughness ---
        float gloss() const { return _gloss; }
        void setGloss(const float value) { _gloss = value; markUniformsDirty(); }
        bool glossInvert() const { return _glossInvert; }
        void setGlossInvert(const bool value) { _glossInvert = value; markUniformsDirty(); }
        // Dynamic grab-pass refraction: transmission samples the mid-frame scene
        // color grab instead of the environment atlas. The camera must have
        // requestSceneColorMap(true) so the grab pass runs.
        bool useDynamicRefraction() const { return _useDynamicRefraction; }
        void setUseDynamicRefraction(const bool value) { _useDynamicRefraction = value; _dirtyShader = true; markUniformsDirty(); }
        // Screen-space reflections: ray-march the reflection against the scene
        // depth grab and sample the scene color grab (needs the camera's
        // requestSceneColorMap + requestSceneDepthMap; the material should be
        // transparent so it draws after the mid-frame grab). Falls back to the
        // reflection probe / env atlas where the ray leaves the screen.
        bool useScreenSpaceReflection() const { return _useSSR; }
        void setUseScreenSpaceReflection(const bool value) { _useSSR = value; _dirtyShader = true; markUniformsDirty(); }
        // Opacity dithering (upstream opacityDither): render partial opacity in the OPAQUE pass
        // by discarding fragments against an ordered Bayer threshold — no sorting artifacts and
        // depth writes stay valid. Leave the material non-transparent when using this.
        //
        // Larger matrices resolve more opacity levels (2x2 gives 4, 16x16 gives 256) at the cost
        // of a coarser, more visible pattern over the surface.
        DitherMode opacityDitherMode() const { return _opacityDitherMode; }
        void setOpacityDitherMode(const DitherMode value)
        {
            markUniformsDirty();
            _opacityDitherMode = value;
            _dirtyShader = true;
        }

        bool opacityDither() const { return _opacityDitherMode != DitherMode::DITHER_NONE; }
        // Convenience for the common case: enable dithering with the default 8x8 matrix.
        void setOpacityDither(const bool value)
        {
            markUniformsDirty();
            setOpacityDitherMode(value ? DitherMode::DITHER_BAYER8 : DitherMode::DITHER_NONE);
        }

        // Independent dither strength (upstream StandardMaterial.alphaDither). Opacity
        // normally drives BOTH alpha blending and dither density; setting this decouples
        // them, so opacity drives only the blend and this value only the dither pattern.
        // Unset (the default) restores the coupled behaviour.
        static constexpr float ALPHA_DITHER_UNSET = -1.0f;
        float alphaDither() const { return _alphaDither; }
        void setAlphaDither(const float value) { _alphaDither = std::max(value, 0.0f); markUniformsDirty(); }
        void clearAlphaDither() { _alphaDither = ALPHA_DITHER_UNSET; markUniformsDirty(); }
        bool hasAlphaDither() const { return _alphaDither >= 0.0f; }

        // Dither the SHADOW pass too (upstream StandardMaterial.opacityShadowDither), so a
        // partially-opaque caster throws a correspondingly thinned shadow instead of a solid
        // one. Independent of opacityDitherMode, exactly as upstream keeps them.
        DitherMode opacityShadowDitherMode() const { return _opacityShadowDitherMode; }
        void setOpacityShadowDitherMode(const DitherMode value)
        {
            markUniformsDirty();
            _opacityShadowDitherMode = value;
            _dirtyShader = true;
        }

        Texture* glossMap() const { return _glossMap; }
        void setGlossMap(Texture* texture) { _glossMap = texture; _dirtyShader = true; markUniformsDirty(); }

        /// Channel of the gloss map that supplies glossiness (upstream glossMapChannel, default "g").
        MapChannel glossMapChannel() const { return _glossMapChannel; }
        void setGlossMapChannel(const MapChannel value) { _glossMapChannel = value; markUniformsDirty(); }

        /// Per-pixel thickness, multiplying the thickness factor (upstream thicknessMap).
        Texture* thicknessMap() const { return _thicknessMap; }
        void setThicknessMap(Texture* texture) { _thicknessMap = texture; _dirtyShader = true; markUniformsDirty(); }
        MapChannel thicknessMapChannel() const { return _thicknessMapChannel; }
        void setThicknessMapChannel(const MapChannel value) { _thicknessMapChannel = value; markUniformsDirty(); }

        /// Per-pixel refraction visibility, multiplying the refraction factor (upstream refractionMap).
        Texture* refractionMap() const { return _refractionMap; }
        void setRefractionMap(Texture* texture) { _refractionMap = texture; _dirtyShader = true; markUniformsDirty(); }
        MapChannel refractionMapChannel() const { return _refractionMapChannel; }
        void setRefractionMapChannel(const MapChannel value) { _refractionMapChannel = value; markUniformsDirty(); }
        // --- Emissive ---
        // StandardMaterial owns the emissive contribution unconditionally: updateUniforms() writes
        // pow(_emissive, 2.2) * _emissiveIntensity to the GPU as linear HDR, overriding whatever
        // base Material::_emissiveFactor the parser populated. This matches upstream's
        // StandardMaterial.emissive semantics and deliberately ignores authoring artifacts like
        // specular-glossiness exporters writing emissiveFactor=(1,1,1) with no emissive texture
        // (which would otherwise produce fully-white glowing walls).
        const Color& emissive() const { return _emissive; }
        void setEmissive(const Color& value) { _emissive = value; markUniformsDirty(); }
        float emissiveIntensity() const { return _emissiveIntensity; }
        void setEmissiveIntensity(const float value) { _emissiveIntensity = value; markUniformsDirty(); }
        Texture* emissiveMap() const { return _emissiveMap; }
        void setEmissiveMap(Texture* texture) { _emissiveMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Vertex color routing (upstream diffuseVertexColor / emissiveVertexColor) ---
        // A mesh's vertex colors modulate the diffuse lane by default, which is how
        // every vertex-colored material in this engine behaved before these existed.
        // Route them to emissive instead for additive stamps like decals, where the
        // color has to survive an unlit, black-diffuse material.
        bool diffuseVertexColor() const { return _diffuseVertexColor; }
        void setDiffuseVertexColor(const bool value) { _diffuseVertexColor = value; markUniformsDirty(); }
        bool emissiveVertexColor() const { return _emissiveVertexColor; }
        void setEmissiveVertexColor(const bool value) { _emissiveVertexColor = value; markUniformsDirty(); }
        // --- Normal ---
        Texture* normalMap() const { return _normalMap; }
        void setNormalMap(Texture* texture) { _normalMap = texture; _dirtyShader = true; markUniformsDirty(); }
        float bumpiness() const { return _bumpiness; }
        void setBumpiness(const float value) { _bumpiness = value; markUniformsDirty(); }
        // --- Opacity ---
        float opacity() const { return _opacity; }
        void setOpacity(const float value) { _opacity = value; markUniformsDirty(); }
        Texture* opacityMap() const { return _opacityMap; }
        void setOpacityMap(Texture* texture) { _opacityMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Height / Parallax ---
        Texture* heightMap() const { return _heightMap; }
        void setHeightMap(Texture* texture) { _heightMap = texture; _dirtyShader = true; markUniformsDirty(); }
        /** Baked lightmap sampled at UV1 and added to indirect diffuse. */
        Texture* lightMap() const { return _lightMap; }
        void setLightMap(Texture* texture) { _lightMap = texture; _dirtyShader = true; markUniformsDirty(); }
        float heightMapFactor() const { return _heightMapFactor; }
        void setHeightMapFactor(const float value) { _heightMapFactor = value; markUniformsDirty(); }
        // --- Anisotropy ---
        float anisotropy() const { return _anisotropy; }
        void setAnisotropy(const float value) { _anisotropy = value; _dirtyShader = true; markUniformsDirty(); }
        // --- Transmission / Refraction ---
        float transmissionFactor() const { return _transmissionFactor; }
        void setTransmissionFactor(const float value) { _transmissionFactor = value; _dirtyShader = true; markUniformsDirty(); }
        float refractionIndex() const { return _refractionIndex; }
        void setRefractionIndex(const float value) { _refractionIndex = value; markUniformsDirty(); }
        float thickness() const { return _thickness; }
        void setThickness(const float value) { _thickness = value; markUniformsDirty(); }
        // KHR_materials_volume Beer-law attenuation. Distance 0 disables (falls
        // back to the legacy baseColor^thickness tint). Runtime uniforms — no
        // shader variant change.
        const Color& attenuationColor() const { return _attenuationColor; }
        void setAttenuationColor(const Color& value) { _attenuationColor = value; markUniformsDirty(); }
        float attenuationDistance() const { return _attenuationDistance; }
        void setAttenuationDistance(const float value) { _attenuationDistance = value; markUniformsDirty(); }
        // KHR_materials_dispersion: per-channel IOR spread (0 = off). Dynamic
        // refraction path only.
        float dispersion() const { return _dispersion; }
        void setDispersion(const float value) { _dispersion = value; markUniformsDirty(); }
        // --- Ambient Occlusion ---
        Texture* aoMap() const { return _aoMap; }
        void setAoMap(Texture* texture) { _aoMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Texture Transforms ---
        const Vector2& diffuseMapTiling() const { return _diffuseMapTiling; }
        void setDiffuseMapTiling(const Vector2& v) { _diffuseMapTiling = v; markUniformsDirty(); }
        const Vector2& diffuseMapOffset() const { return _diffuseMapOffset; }
        void setDiffuseMapOffset(const Vector2& v) { _diffuseMapOffset = v; markUniformsDirty(); }
        float diffuseMapRotation() const { return _diffuseMapRotation; }
        void setDiffuseMapRotation(float deg) { _diffuseMapRotation = deg; markUniformsDirty(); }
        const Vector2& normalMapTiling() const { return _normalMapTiling; }
        void setNormalMapTiling(const Vector2& v) { _normalMapTiling = v; markUniformsDirty(); }
        const Vector2& normalMapOffset() const { return _normalMapOffset; }
        void setNormalMapOffset(const Vector2& v) { _normalMapOffset = v; markUniformsDirty(); }
        float normalMapRotation() const { return _normalMapRotation; }
        void setNormalMapRotation(float deg) { _normalMapRotation = deg; markUniformsDirty(); }
        const Vector2& metalnessMapTiling() const { return _metalnessMapTiling; }
        void setMetalnessMapTiling(const Vector2& v) { _metalnessMapTiling = v; markUniformsDirty(); }
        const Vector2& metalnessMapOffset() const { return _metalnessMapOffset; }
        void setMetalnessMapOffset(const Vector2& v) { _metalnessMapOffset = v; markUniformsDirty(); }
        float metalnessMapRotation() const { return _metalnessMapRotation; }
        void setMetalnessMapRotation(float deg) { _metalnessMapRotation = deg; markUniformsDirty(); }
        const Vector2& aoMapTiling() const { return _aoMapTiling; }
        void setAoMapTiling(const Vector2& v) { _aoMapTiling = v; markUniformsDirty(); }
        const Vector2& aoMapOffset() const { return _aoMapOffset; }
        void setAoMapOffset(const Vector2& v) { _aoMapOffset = v; markUniformsDirty(); }
        float aoMapRotation() const { return _aoMapRotation; }
        void setAoMapRotation(float deg) { _aoMapRotation = deg; markUniformsDirty(); }
        const Vector2& emissiveMapTiling() const { return _emissiveMapTiling; }
        void setEmissiveMapTiling(const Vector2& v) { _emissiveMapTiling = v; markUniformsDirty(); }
        const Vector2& emissiveMapOffset() const { return _emissiveMapOffset; }
        void setEmissiveMapOffset(const Vector2& v) { _emissiveMapOffset = v; markUniformsDirty(); }
        float emissiveMapRotation() const { return _emissiveMapRotation; }
        void setEmissiveMapRotation(float deg) { _emissiveMapRotation = deg; markUniformsDirty(); }
        // --- Rendering flags ---
        bool useFog() const { return _useFog; }
        void setUseFog(const bool value) { _useFog = value; _dirtyShader = true; markUniformsDirty(); }
        bool useLighting() const { return _useLighting; }
        void setUseLighting(const bool value) { _useLighting = value; _dirtyShader = true; markUniformsDirty(); }
        bool useSkybox() const { return _useSkybox; }
        void setUseSkybox(const bool value) { _useSkybox = value; _dirtyShader = true; markUniformsDirty(); }
        bool twoSidedLighting() const { return _twoSidedLighting; }
        void setTwoSidedLighting(const bool value) { _twoSidedLighting = value; _dirtyShader = true; markUniformsDirty(); }
        // --- Planar Reflection ---
        // DEVIATION: planar reflection is handled at the application level as a script.
        // We promote it to a material property for simpler integration with the shader variant system.
        Texture* reflectionMap() const { return _reflectionMap; }
        void setReflectionMap(Texture* texture) { _reflectionMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Clearcoat ---
        // dual-layer clearcoat material (KHR_materials_clearcoat).
        // A thin dielectric coat (IOR 1.5, F0=0.04) over the standard PBR base.
        float clearCoat() const { return _clearCoat; }
        void setClearCoat(const float value) { _clearCoat = value; _dirtyShader = true; markUniformsDirty(); }
        float clearCoatGloss() const { return _clearCoatGloss; }
        void setClearCoatGloss(const float value) { _clearCoatGloss = value; markUniformsDirty(); }
        bool clearCoatGlossInvert() const { return _clearCoatGlossInvert; }
        void setClearCoatGlossInvert(const bool value) { _clearCoatGlossInvert = value; markUniformsDirty(); }
        float clearCoatBumpiness() const { return _clearCoatBumpiness; }
        void setClearCoatBumpiness(const float value) { _clearCoatBumpiness = value; markUniformsDirty(); }
        Texture* clearCoatMap() const { return _clearCoatMap; }
        void setClearCoatMap(Texture* texture) { _clearCoatMap = texture; _dirtyShader = true; markUniformsDirty(); }
        Texture* clearCoatGlossMap() const { return _clearCoatGlossMap; }
        void setClearCoatGlossMap(Texture* texture) { _clearCoatGlossMap = texture; _dirtyShader = true; markUniformsDirty(); }
        Texture* clearCoatNormalMap() const { return _clearCoatNormalMap; }
        void setClearCoatNormalMap(Texture* texture) { _clearCoatNormalMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Sheen (KHR_materials_sheen) ---
        // fabric/velvet sheen layer (Charlie sheen BRDF).
        const Color& sheenColor() const { return _sheenColor; }
        void setSheenColor(const Color& value) { _sheenColor = value; _dirtyShader = true; markUniformsDirty(); }
        float sheenRoughness() const { return _sheenRoughness; }
        void setSheenRoughness(const float value) { _sheenRoughness = value; markUniformsDirty(); }
        Texture* sheenMap() const { return _sheenMap; }
        void setSheenMap(Texture* texture) { _sheenMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Iridescence (KHR_materials_iridescence) ---
        // thin-film interference layer.
        float iridescenceIntensity() const { return _iridescenceIntensity; }
        void setIridescenceIntensity(const float value) { _iridescenceIntensity = value; _dirtyShader = true; markUniformsDirty(); }
        float iridescenceIOR() const { return _iridescenceIOR; }
        void setIridescenceIOR(const float value) { _iridescenceIOR = value; markUniformsDirty(); }
        float iridescenceThicknessMin() const { return _iridescenceThicknessMin; }
        void setIridescenceThicknessMin(const float value) { _iridescenceThicknessMin = value; markUniformsDirty(); }
        float iridescenceThicknessMax() const { return _iridescenceThicknessMax; }
        void setIridescenceThicknessMax(const float value) { _iridescenceThicknessMax = value; markUniformsDirty(); }
        Texture* iridescenceMap() const { return _iridescenceMap; }
        void setIridescenceMap(Texture* texture) { _iridescenceMap = texture; _dirtyShader = true; markUniformsDirty(); }
        Texture* iridescenceThicknessMap() const { return _iridescenceThicknessMap; }
        void setIridescenceThicknessMap(Texture* texture) { _iridescenceThicknessMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Spec-Gloss (KHR_materials_pbrSpecularGlossiness) ---
        // alternative PBR parameterization (specular + glossiness).
        // Enables the spec-gloss parameterization even without a specGlossMap
        // (factor-only KHR_materials_pbrSpecularGlossiness materials).
        bool useSpecGloss() const { return _useSpecGloss; }
        void setUseSpecGloss(const bool value) { _useSpecGloss = value; _dirtyShader = true; markUniformsDirty(); }
        const Color& specularColor() const { return _specularColor; }
        void setSpecularColor(const Color& value) { _specularColor = value; markUniformsDirty(); }
        float glossiness() const { return _glossiness; }
        void setGlossiness(const float value) { _glossiness = value; markUniformsDirty(); }
        Texture* specGlossMap() const { return _specGlossMap; }
        void setSpecGlossMap(Texture* texture) { _specGlossMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Detail Normals ---
        // detail normal map overlay blended with primary normal.
        float detailNormalScale() const { return _detailNormalScale; }
        void setDetailNormalScale(const float value) { _detailNormalScale = value; markUniformsDirty(); }
        Texture* detailNormalMap() const { return _detailNormalMap; }
        void setDetailNormalMap(Texture* texture) { _detailNormalMap = texture; _dirtyShader = true; markUniformsDirty(); }
        const TextureTransform& detailNormalTransform() const { return _detailNormalTransform; }
        void setDetailNormalTransform(const TextureTransform& t) { _detailNormalTransform = t; markUniformsDirty(); }
        // --- Displacement ---
        // vertex displacement along normals.
        float displacementScale() const { return _displacementScale; }
        void setDisplacementScale(const float value) { _displacementScale = value; _dirtyShader = true; markUniformsDirty(); }
        float displacementBias() const { return _displacementBias; }
        void setDisplacementBias(const float value) { _displacementBias = value; markUniformsDirty(); }
        Texture* displacementMap() const { return _displacementMap; }
        void setDisplacementMap(Texture* texture) { _displacementMap = texture; _dirtyShader = true; markUniformsDirty(); }
        // --- Oren-Nayar ---
        // roughness-dependent diffuse model (alternative to Lambertian).
        bool useOrenNayar() const { return _useOrenNayar; }
        void setUseOrenNayar(const bool value) { _useOrenNayar = value; _dirtyShader = true; markUniformsDirty(); }
        // when true, the material accumulates shadow factors and outputs
        // them via multiplicative blending (LIT_SHADOW_CATCHER shader path).
        bool shadowCatcher() const { return _shadowCatcher; }
        void setShadowCatcher(const bool value) { _shadowCatcher = value; _dirtyShader = true; markUniformsDirty(); }
        bool dirtyShader() const { return _dirtyShader; }
        void clearDirtyShader() { _dirtyShader = false; markUniformsDirty(); }
    private:
        Color _diffuse = Color(1.0f, 1.0f, 1.0f, 1.0f);
        Texture* _diffuseMap = nullptr;

        Color _specular = Color(0.0f, 0.0f, 0.0f, 1.0f);

        float _metalness = 0.0f;
        bool _useMetalness = true;
        Texture* _metalnessMap = nullptr;

        float _gloss = 0.25f;
        bool _glossInvert = false;
        bool _useDynamicRefraction = false;
        bool _useSSR = false;
        DitherMode _opacityDitherMode = DitherMode::DITHER_NONE;
        DitherMode _opacityShadowDitherMode = DitherMode::DITHER_NONE;
        float _alphaDither = ALPHA_DITHER_UNSET;
        Texture* _glossMap = nullptr;
        MapChannel _glossMapChannel = MapChannel::MAP_CHANNEL_G;
        Texture* _thicknessMap = nullptr;
        MapChannel _thicknessMapChannel = MapChannel::MAP_CHANNEL_G;
        Texture* _refractionMap = nullptr;
        MapChannel _refractionMapChannel = MapChannel::MAP_CHANNEL_G;

        Color _emissive = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float _emissiveIntensity = 1.0f;
        bool _diffuseVertexColor = true;
        bool _emissiveVertexColor = false;
        Texture* _emissiveMap = nullptr;

        Texture* _normalMap = nullptr;
        float _bumpiness = 1.0f;

        float _opacity = 1.0f;
        Texture* _opacityMap = nullptr;

        Texture* _heightMap = nullptr;
        Texture* _lightMap = nullptr;
        float _heightMapFactor = 0.05f;

        float _anisotropy = 0.0f;

        float _transmissionFactor = 0.0f;
        float _refractionIndex = 1.5f;
        float _thickness = 0.0f;
        Color _attenuationColor{1.0f, 1.0f, 1.0f, 1.0f};
        float _attenuationDistance = 0.0f;
        float _dispersion = 0.0f;

        Texture* _aoMap = nullptr;

        // Per-map texture transforms.
        Vector2 _diffuseMapTiling{1.0f, 1.0f};
        Vector2 _diffuseMapOffset{0.0f, 0.0f};
        float _diffuseMapRotation = 0.0f;

        Vector2 _normalMapTiling{1.0f, 1.0f};
        Vector2 _normalMapOffset{0.0f, 0.0f};
        float _normalMapRotation = 0.0f;

        Vector2 _metalnessMapTiling{1.0f, 1.0f};
        Vector2 _metalnessMapOffset{0.0f, 0.0f};
        float _metalnessMapRotation = 0.0f;

        Vector2 _aoMapTiling{1.0f, 1.0f};
        Vector2 _aoMapOffset{0.0f, 0.0f};
        float _aoMapRotation = 0.0f;

        Vector2 _emissiveMapTiling{1.0f, 1.0f};
        Vector2 _emissiveMapOffset{0.0f, 0.0f};
        float _emissiveMapRotation = 0.0f;

        Texture* _reflectionMap = nullptr;

        // clearcoat properties.
        float _clearCoat = 0.0f;
        float _clearCoatGloss = 1.0f;
        bool _clearCoatGlossInvert = false;
        float _clearCoatBumpiness = 1.0f;
        Texture* _clearCoatMap = nullptr;
        Texture* _clearCoatGlossMap = nullptr;
        Texture* _clearCoatNormalMap = nullptr;

        // sheen properties (KHR_materials_sheen).
        Color _sheenColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float _sheenRoughness = 0.0f;
        Texture* _sheenMap = nullptr;

        // iridescence properties (KHR_materials_iridescence).
        float _iridescenceIntensity = 0.0f;
        float _iridescenceIOR = 1.3f;
        float _iridescenceThicknessMin = 100.0f;
        float _iridescenceThicknessMax = 400.0f;
        Texture* _iridescenceMap = nullptr;
        Texture* _iridescenceThicknessMap = nullptr;

        // spec-gloss properties (KHR_materials_pbrSpecularGlossiness).
        Color _specularColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float _glossiness = 1.0f;
        Texture* _specGlossMap = nullptr;

        // detail normal map properties.
        float _detailNormalScale = 1.0f;
        Texture* _detailNormalMap = nullptr;
        TextureTransform _detailNormalTransform;

        // displacement properties.
        float _displacementScale = 0.0f;
        float _displacementBias = 0.5f;
        Texture* _displacementMap = nullptr;

        // Oren-Nayar diffuse model toggle.
        bool _useOrenNayar = false;
        bool _useSpecGloss = false;

        bool _useFog = true;
        bool _useLighting = true;
        bool _useSkybox = true;
        bool _twoSidedLighting = false;
        bool _shadowCatcher = false;

        mutable bool _dirtyShader = true;
    };
}
