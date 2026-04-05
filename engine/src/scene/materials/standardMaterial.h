// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#pragma once
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

        void updateUniforms(MaterialUniforms& uniforms) const override;
        void getTextureSlots(std::vector<TextureSlot>& slots) const override;

        // --- Diffuse ---
        const Color& diffuse() const { return _diffuse; }
        void setDiffuse(const Color& value) { _diffuse = value; }
        Texture* diffuseMap() const { return _diffuseMap; }
        void setDiffuseMap(Texture* texture) { _diffuseMap = texture; _dirtyShader = true; }

        // --- Specular ---
        const Color& specular() const { return _specular; }
        void setSpecular(const Color& value) { _specular = value; }

        // --- Metalness ---
        float metalness() const { return _metalness; }
        void setMetalness(const float value) { _metalness = value; }
        bool useMetalness() const { return _useMetalness; }
        void setUseMetalness(const bool value) { _useMetalness = value; _dirtyShader = true; }
        Texture* metalnessMap() const { return _metalnessMap; }
        void setMetalnessMap(Texture* texture) { _metalnessMap = texture; _dirtyShader = true; }

        // --- Gloss / Roughness ---
        float gloss() const { return _gloss; }
        void setGloss(const float value) { _gloss = value; }
        bool glossInvert() const { return _glossInvert; }
        void setGlossInvert(const bool value) { _glossInvert = value; }
        Texture* glossMap() const { return _glossMap; }
        void setGlossMap(Texture* texture) { _glossMap = texture; _dirtyShader = true; }

        // --- Emissive ---
        const Color& emissive() const { return _emissive; }
        // Setting any of the three emissive controls (color/intensity/map) flips _emissiveSet so
        // that updateUniforms() knows the caller is driving emission via the StandardMaterial API
        // and should override whatever base Material::_emissiveFactor the parser wrote. Without
        // this opt-in, the decoupled override would fire on parsers that set _emissive as part of
        // material init (e.g. Assimp/OBJ parsers) and silently re-write emissive for materials
        // the caller never meant to touch.
        void setEmissive(const Color& value) { _emissive = value; _emissiveSet = true; }
        float emissiveIntensity() const { return _emissiveIntensity; }
        void setEmissiveIntensity(const float value) { _emissiveIntensity = value; _emissiveSet = true; }
        Texture* emissiveMap() const { return _emissiveMap; }
        void setEmissiveMap(Texture* texture) { _emissiveMap = texture; _emissiveSet = true; _dirtyShader = true; }
        bool emissiveSet() const { return _emissiveSet; }
        void clearEmissiveOverride() { _emissiveSet = false; }

        // --- Normal ---
        Texture* normalMap() const { return _normalMap; }
        void setNormalMap(Texture* texture) { _normalMap = texture; _dirtyShader = true; }
        float bumpiness() const { return _bumpiness; }
        void setBumpiness(const float value) { _bumpiness = value; }

        // --- Opacity ---
        float opacity() const { return _opacity; }
        void setOpacity(const float value) { _opacity = value; }
        Texture* opacityMap() const { return _opacityMap; }
        void setOpacityMap(Texture* texture) { _opacityMap = texture; _dirtyShader = true; }

        // --- Height / Parallax ---
        Texture* heightMap() const { return _heightMap; }
        void setHeightMap(Texture* texture) { _heightMap = texture; _dirtyShader = true; }
        float heightMapFactor() const { return _heightMapFactor; }
        void setHeightMapFactor(const float value) { _heightMapFactor = value; }

        // --- Anisotropy ---
        float anisotropy() const { return _anisotropy; }
        void setAnisotropy(const float value) { _anisotropy = value; _dirtyShader = true; }

        // --- Transmission / Refraction ---
        float transmissionFactor() const { return _transmissionFactor; }
        void setTransmissionFactor(const float value) { _transmissionFactor = value; _dirtyShader = true; }
        float refractionIndex() const { return _refractionIndex; }
        void setRefractionIndex(const float value) { _refractionIndex = value; }
        float thickness() const { return _thickness; }
        void setThickness(const float value) { _thickness = value; }

        // --- Ambient Occlusion ---
        Texture* aoMap() const { return _aoMap; }
        void setAoMap(Texture* texture) { _aoMap = texture; _dirtyShader = true; }

        // --- Texture Transforms ---
        const Vector2& diffuseMapTiling() const { return _diffuseMapTiling; }
        void setDiffuseMapTiling(const Vector2& v) { _diffuseMapTiling = v; }
        const Vector2& diffuseMapOffset() const { return _diffuseMapOffset; }
        void setDiffuseMapOffset(const Vector2& v) { _diffuseMapOffset = v; }
        float diffuseMapRotation() const { return _diffuseMapRotation; }
        void setDiffuseMapRotation(float deg) { _diffuseMapRotation = deg; }

        const Vector2& normalMapTiling() const { return _normalMapTiling; }
        void setNormalMapTiling(const Vector2& v) { _normalMapTiling = v; }
        const Vector2& normalMapOffset() const { return _normalMapOffset; }
        void setNormalMapOffset(const Vector2& v) { _normalMapOffset = v; }
        float normalMapRotation() const { return _normalMapRotation; }
        void setNormalMapRotation(float deg) { _normalMapRotation = deg; }

        const Vector2& metalnessMapTiling() const { return _metalnessMapTiling; }
        void setMetalnessMapTiling(const Vector2& v) { _metalnessMapTiling = v; }
        const Vector2& metalnessMapOffset() const { return _metalnessMapOffset; }
        void setMetalnessMapOffset(const Vector2& v) { _metalnessMapOffset = v; }
        float metalnessMapRotation() const { return _metalnessMapRotation; }
        void setMetalnessMapRotation(float deg) { _metalnessMapRotation = deg; }

        const Vector2& aoMapTiling() const { return _aoMapTiling; }
        void setAoMapTiling(const Vector2& v) { _aoMapTiling = v; }
        const Vector2& aoMapOffset() const { return _aoMapOffset; }
        void setAoMapOffset(const Vector2& v) { _aoMapOffset = v; }
        float aoMapRotation() const { return _aoMapRotation; }
        void setAoMapRotation(float deg) { _aoMapRotation = deg; }

        const Vector2& emissiveMapTiling() const { return _emissiveMapTiling; }
        void setEmissiveMapTiling(const Vector2& v) { _emissiveMapTiling = v; }
        const Vector2& emissiveMapOffset() const { return _emissiveMapOffset; }
        void setEmissiveMapOffset(const Vector2& v) { _emissiveMapOffset = v; }
        float emissiveMapRotation() const { return _emissiveMapRotation; }
        void setEmissiveMapRotation(float deg) { _emissiveMapRotation = deg; }

        // --- Rendering flags ---
        bool useFog() const { return _useFog; }
        void setUseFog(const bool value) { _useFog = value; _dirtyShader = true; }
        bool useLighting() const { return _useLighting; }
        void setUseLighting(const bool value) { _useLighting = value; _dirtyShader = true; }
        bool useSkybox() const { return _useSkybox; }
        void setUseSkybox(const bool value) { _useSkybox = value; _dirtyShader = true; }
        bool twoSidedLighting() const { return _twoSidedLighting; }
        void setTwoSidedLighting(const bool value) { _twoSidedLighting = value; _dirtyShader = true; }

        // --- Planar Reflection ---
        // DEVIATION: planar reflection is handled at the application level as a script.
        // We promote it to a material property for simpler integration with the shader variant system.
        Texture* reflectionMap() const { return _reflectionMap; }
        void setReflectionMap(Texture* texture) { _reflectionMap = texture; _dirtyShader = true; }

        // --- Clearcoat ---
        // dual-layer clearcoat material (KHR_materials_clearcoat).
        // A thin dielectric coat (IOR 1.5, F0=0.04) over the standard PBR base.
        float clearCoat() const { return _clearCoat; }
        void setClearCoat(const float value) { _clearCoat = value; _dirtyShader = true; }
        float clearCoatGloss() const { return _clearCoatGloss; }
        void setClearCoatGloss(const float value) { _clearCoatGloss = value; }
        bool clearCoatGlossInvert() const { return _clearCoatGlossInvert; }
        void setClearCoatGlossInvert(const bool value) { _clearCoatGlossInvert = value; }
        float clearCoatBumpiness() const { return _clearCoatBumpiness; }
        void setClearCoatBumpiness(const float value) { _clearCoatBumpiness = value; }
        Texture* clearCoatMap() const { return _clearCoatMap; }
        void setClearCoatMap(Texture* texture) { _clearCoatMap = texture; _dirtyShader = true; }
        Texture* clearCoatGlossMap() const { return _clearCoatGlossMap; }
        void setClearCoatGlossMap(Texture* texture) { _clearCoatGlossMap = texture; _dirtyShader = true; }
        Texture* clearCoatNormalMap() const { return _clearCoatNormalMap; }
        void setClearCoatNormalMap(Texture* texture) { _clearCoatNormalMap = texture; _dirtyShader = true; }

        // --- Sheen (KHR_materials_sheen) ---
        // fabric/velvet sheen layer (Charlie sheen BRDF).
        const Color& sheenColor() const { return _sheenColor; }
        void setSheenColor(const Color& value) { _sheenColor = value; _dirtyShader = true; }
        float sheenRoughness() const { return _sheenRoughness; }
        void setSheenRoughness(const float value) { _sheenRoughness = value; }
        Texture* sheenMap() const { return _sheenMap; }
        void setSheenMap(Texture* texture) { _sheenMap = texture; _dirtyShader = true; }

        // --- Iridescence (KHR_materials_iridescence) ---
        // thin-film interference layer.
        float iridescenceIntensity() const { return _iridescenceIntensity; }
        void setIridescenceIntensity(const float value) { _iridescenceIntensity = value; _dirtyShader = true; }
        float iridescenceIOR() const { return _iridescenceIOR; }
        void setIridescenceIOR(const float value) { _iridescenceIOR = value; }
        float iridescenceThicknessMin() const { return _iridescenceThicknessMin; }
        void setIridescenceThicknessMin(const float value) { _iridescenceThicknessMin = value; }
        float iridescenceThicknessMax() const { return _iridescenceThicknessMax; }
        void setIridescenceThicknessMax(const float value) { _iridescenceThicknessMax = value; }
        Texture* iridescenceMap() const { return _iridescenceMap; }
        void setIridescenceMap(Texture* texture) { _iridescenceMap = texture; _dirtyShader = true; }
        Texture* iridescenceThicknessMap() const { return _iridescenceThicknessMap; }
        void setIridescenceThicknessMap(Texture* texture) { _iridescenceThicknessMap = texture; _dirtyShader = true; }

        // --- Spec-Gloss (KHR_materials_pbrSpecularGlossiness) ---
        // alternative PBR parameterization (specular + glossiness).
        const Color& specularColor() const { return _specularColor; }
        void setSpecularColor(const Color& value) { _specularColor = value; }
        float glossiness() const { return _glossiness; }
        void setGlossiness(const float value) { _glossiness = value; }
        Texture* specGlossMap() const { return _specGlossMap; }
        void setSpecGlossMap(Texture* texture) { _specGlossMap = texture; _dirtyShader = true; }

        // --- Detail Normals ---
        // detail normal map overlay blended with primary normal.
        float detailNormalScale() const { return _detailNormalScale; }
        void setDetailNormalScale(const float value) { _detailNormalScale = value; }
        Texture* detailNormalMap() const { return _detailNormalMap; }
        void setDetailNormalMap(Texture* texture) { _detailNormalMap = texture; _dirtyShader = true; }
        const TextureTransform& detailNormalTransform() const { return _detailNormalTransform; }
        void setDetailNormalTransform(const TextureTransform& t) { _detailNormalTransform = t; }

        // --- Displacement ---
        // vertex displacement along normals.
        float displacementScale() const { return _displacementScale; }
        void setDisplacementScale(const float value) { _displacementScale = value; _dirtyShader = true; }
        float displacementBias() const { return _displacementBias; }
        void setDisplacementBias(const float value) { _displacementBias = value; }
        Texture* displacementMap() const { return _displacementMap; }
        void setDisplacementMap(Texture* texture) { _displacementMap = texture; _dirtyShader = true; }

        // --- Oren-Nayar ---
        // roughness-dependent diffuse model (alternative to Lambertian).
        bool useOrenNayar() const { return _useOrenNayar; }
        void setUseOrenNayar(const bool value) { _useOrenNayar = value; _dirtyShader = true; }

        // when true, the material accumulates shadow factors and outputs
        // them via multiplicative blending (LIT_SHADOW_CATCHER shader path).
        bool shadowCatcher() const { return _shadowCatcher; }
        void setShadowCatcher(const bool value) { _shadowCatcher = value; _dirtyShader = true; }

        bool dirtyShader() const { return _dirtyShader; }
        void clearDirtyShader() { _dirtyShader = false; }

    private:
        Color _diffuse = Color(1.0f, 1.0f, 1.0f, 1.0f);
        Texture* _diffuseMap = nullptr;

        Color _specular = Color(0.0f, 0.0f, 0.0f, 1.0f);

        float _metalness = 0.0f;
        bool _useMetalness = true;
        Texture* _metalnessMap = nullptr;

        float _gloss = 0.25f;
        bool _glossInvert = false;
        Texture* _glossMap = nullptr;

        Color _emissive = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float _emissiveIntensity = 1.0f;
        Texture* _emissiveMap = nullptr;
        // True when a caller used the StandardMaterial emissive API (setEmissive/Intensity/Map)
        // after construction. Used by updateUniforms() to decide whether to override the base
        // Material::_emissiveFactor written by the GLB parser.
        bool _emissiveSet = false;

        Texture* _normalMap = nullptr;
        float _bumpiness = 1.0f;

        float _opacity = 1.0f;
        Texture* _opacityMap = nullptr;

        Texture* _heightMap = nullptr;
        float _heightMapFactor = 0.05f;

        float _anisotropy = 0.0f;

        float _transmissionFactor = 0.0f;
        float _refractionIndex = 1.5f;
        float _thickness = 0.0f;

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

        bool _useFog = true;
        bool _useLighting = true;
        bool _useSkybox = true;
        bool _twoSidedLighting = false;
        bool _shadowCatcher = false;

        mutable bool _dirtyShader = true;
    };
}
