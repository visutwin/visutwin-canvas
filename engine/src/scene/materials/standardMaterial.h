// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025
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
        // Writes through to the base Material's slot, as setAoMap does: a glTF
        // material binds the BASE slot, so a StandardMaterial-only store made
        // `setDiffuseMap(nullptr)` clear nothing on a loaded material.
        Texture* diffuseMap() const { return _diffuseMap ? _diffuseMap : baseColorTexture(); }
        void setDiffuseMap(Texture* texture)
        {
            _diffuseMap = texture;
            _msdfMap = nullptr;   // one base slot: the last of the two setters wins
            setBaseColorTexture(texture);
            setHasBaseColorTexture(texture != nullptr);
            markUniformsDirty();
        }
        // --- Specular ---
        /// Specular colour of the SPECULAR workflow (useMetalness false), authored in
        /// sRGB like `diffuse` and uploaded linear. Black, the default, means no
        /// specular at all — see rendersSpecular().
        const Color& specular() const { return _specular; }
        void setSpecular(const Color& value) { _specular = value; markUniformsDirty(); }
        /// In the METALNESS workflow, tint the
        /// non-metal F0 by `specular` (KHR_materials_specular's specularColorFactor).
        /// Off by default, when the non-metal F0 is untinted.
        bool useMetalnessSpecularColor() const { return _useMetalnessSpecularColor; }
        void setUseMetalnessSpecularColor(const bool value) { _useMetalnessSpecularColor = value; markUniformsDirty(); }
        /// KHR_materials_specular's specularFactor: scales
        /// the non-metal F0 in the metalness workflow. Default 1.
        float specularityFactor() const { return _specularityFactor; }
        void setSpecularityFactor(const float value) { _specularityFactor = value; markUniformsDirty(); }
        // --- Metalness ---
        float metalness() const { return _metalness; }
        void setMetalness(const float value) { _metalness = value; markUniformsDirty(); }
        /// Selects the workflow. Default FALSE, so a material is
        /// in the specular workflow (specular colour + gloss) until it asks for
        /// metalness, and `metalness` (default 1) only applies once it does.
        bool useMetalness() const { return _useMetalness; }
        void setUseMetalness(const bool value) { _useMetalness = value; markUniformsDirty(); }

        /// The specular workflow is active: F0 comes from `specular`, metalness is 0.
        bool usesSpecularWorkflow() const { return !_useMetalness; }

        /// Whether this material renders ANY specular — direct, area, clustered or
        /// reflected. A specular-workflow material with a black specular colour, no specular map
        /// and no clearcoat gets none, not merely a dark one. A black F0 is not the
        /// same thing: the Fresnel term still reflects at grazing angles, which is why
        /// this is a shader variant (VT_FEATURE_NO_SPECULAR) rather than a colour.
        bool rendersSpecular() const
        {
            return _useMetalness || _specGlossMap != nullptr || _clearCoat > 0.0f ||
                _specular.r > 0.0f || _specular.g > 0.0f || _specular.b > 0.0f;
        }
        // Writes through to the base Material's slot, as setAoMap does: a glTF
        // material binds the BASE slot, so a StandardMaterial-only store made
        // `setMetalnessMap(nullptr)` clear nothing on a loaded material.
        Texture* metalnessMap() const { return _metalnessMap ? _metalnessMap : metallicRoughnessTexture(); }
        void setMetalnessMap(Texture* texture)
        {
            _metalnessMap = texture;
            setMetallicRoughnessTexture(texture);
            setHasMetallicRoughnessTexture(texture != nullptr);
            markUniformsDirty();
        }
        // --- Gloss / Roughness ---
        float gloss() const { return _gloss; }
        void setGloss(const float value) { _gloss = value; markUniformsDirty(); }
        bool glossInvert() const { return _glossInvert; }
        void setGlossInvert(const bool value) { _glossInvert = value; markUniformsDirty(); }
        // Dynamic grab-pass refraction: transmission samples the mid-frame scene
        // color grab instead of the environment atlas. The camera must have
        // requestSceneColorMap(true) so the grab pass runs.
        bool useDynamicRefraction() const { return _useDynamicRefraction; }
        void setUseDynamicRefraction(const bool value) { _useDynamicRefraction = value; markUniformsDirty(); }
        // Screen-space reflections: ray-march the reflection against the scene
        // depth grab and sample the scene color grab (needs the camera's
        // requestSceneColorMap + requestSceneDepthMap; the material should be
        // transparent so it draws after the mid-frame grab). Falls back to the
        // reflection probe / env atlas where the ray leaves the screen.
        bool useScreenSpaceReflection() const { return _useSSR; }
        void setUseScreenSpaceReflection(const bool value) { _useSSR = value; markUniformsDirty(); }
        // Opacity dithering: render partial opacity in the OPAQUE pass
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
        }

        bool opacityDither() const { return _opacityDitherMode != DitherMode::DITHER_NONE; }
        // Convenience for the common case: enable dithering with the default 8x8 matrix.
        void setOpacityDither(const bool value)
        {
            markUniformsDirty();
            setOpacityDitherMode(value ? DitherMode::DITHER_BAYER8 : DitherMode::DITHER_NONE);
        }

        // Independent dither strength. Opacity
        // normally drives BOTH alpha blending and dither density; setting this decouples
        // them, so opacity drives only the blend and this value only the dither pattern.
        // Unset (the default) restores the coupled behaviour.
        static constexpr float ALPHA_DITHER_UNSET = -1.0f;
        float alphaDither() const { return _alphaDither; }
        void setAlphaDither(const float value) { _alphaDither = std::max(value, 0.0f); markUniformsDirty(); }
        void clearAlphaDither() { _alphaDither = ALPHA_DITHER_UNSET; markUniformsDirty(); }
        bool hasAlphaDither() const { return _alphaDither >= 0.0f; }

        // Dither the SHADOW pass too, so a
        // partially-opaque caster throws a correspondingly thinned shadow instead of a solid
        // one. Independent of opacityDitherMode.
        DitherMode opacityShadowDitherMode() const { return _opacityShadowDitherMode; }
        void setOpacityShadowDitherMode(const DitherMode value)
        {
            markUniformsDirty();
            _opacityShadowDitherMode = value;
        }

        Texture* glossMap() const { return _glossMap; }
        void setGlossMap(Texture* texture) { _glossMap = texture; markUniformsDirty(); }

        /// Channel of the gloss map that supplies glossiness (default "g").
        MapChannel glossMapChannel() const { return _glossMapChannel; }
        void setGlossMapChannel(const MapChannel value) { _glossMapChannel = value; markUniformsDirty(); }

        /// Per-pixel thickness, multiplying the thickness factor.
        Texture* thicknessMap() const { return _thicknessMap; }
        void setThicknessMap(Texture* texture) { _thicknessMap = texture; markUniformsDirty(); }
        MapChannel thicknessMapChannel() const { return _thicknessMapChannel; }
        void setThicknessMapChannel(const MapChannel value) { _thicknessMapChannel = value; markUniformsDirty(); }

        /// Per-pixel refraction visibility, multiplying the refraction factor.
        Texture* refractionMap() const { return _refractionMap; }
        void setRefractionMap(Texture* texture) { _refractionMap = texture; markUniformsDirty(); }
        MapChannel refractionMapChannel() const { return _refractionMapChannel; }
        void setRefractionMapChannel(const MapChannel value) { _refractionMapChannel = value; markUniformsDirty(); }
        // --- Emissive ---
        // StandardMaterial owns the emissive contribution unconditionally: updateUniforms() writes
        // pow(_emissive, 2.2) * _emissiveIntensity to the GPU as linear HDR, overriding whatever
        // base Material::_emissiveFactor the parser populated. This deliberately ignores
        // authoring artifacts like
        // specular-glossiness exporters writing emissiveFactor=(1,1,1) with no emissive texture
        // (which would otherwise produce fully-white glowing walls).
        /// Tint of the AMBIENT diffuse, authored in sRGB like
        /// `diffuse` and uploaded linear; white (the default) changes nothing. It does
        /// not touch a lightmap, which replaces the ambient, nor direct light.
        const Color& ambient() const { return _ambient; }
        void setAmbient(const Color& value) { _ambient = value; markUniformsDirty(); }

        // --- MSDF text ---
        /// A multi-channel signed distance field atlas page. It takes the BASE COLOUR
        /// slot — a text material has no diffuse map — and compiles VT_FEATURE_MSDF, under
        /// which the unlit path reads the slot as distances rather than colour: coverage
        /// from the median of RGB, then outline and shadow composited in linear.
        /// Setting it replaces any diffuse map, and vice versa.
        Texture* msdfMap() const { return _msdfMap; }
        void setMsdfMap(Texture* texture)
        {
            _msdfMap = texture;
            _diffuseMap = nullptr;
            setBaseColorTexture(texture);
            setHasBaseColorTexture(texture != nullptr);
            markUniformsDirty();
        }
        /// Texels of distance spread in the atlas and the intensity that fattens the
        /// glyph.
        void setMsdfFont(const float pxRange, const float intensity)
        {
            _msdfPxRange = pxRange;
            _msdfIntensity = intensity;
            markUniformsDirty();
        }
        /// Outline colour (sRGB, alpha straight) and SHADER thickness (the
        /// element's outlineThickness x 0.2).
        void setMsdfOutline(const Color& color, const float thickness)
        {
            _msdfOutlineColor = color;
            _msdfOutlineThickness = thickness;
            markUniformsDirty();
        }
        /// Shadow colour (sRGB, alpha straight) and offset in atlas UV (the
        /// text element converts its shadowOffset).
        void setMsdfShadow(const Color& color, const Vector2& uvOffset)
        {
            _msdfShadowColor = color;
            _msdfShadowOffset = uvOffset;
            markUniformsDirty();
        }

        const Color& emissive() const { return _emissive; }
        void setEmissive(const Color& value) { _emissive = value; markUniformsDirty(); }
        float emissiveIntensity() const { return _emissiveIntensity; }
        void setEmissiveIntensity(const float value) { _emissiveIntensity = value; markUniformsDirty(); }
        // Writes through to the base Material's slot, as setAoMap does: a glTF
        // material binds the BASE slot, so a StandardMaterial-only store made
        // `setEmissiveMap(nullptr)` clear nothing on a loaded material.
        Texture* emissiveMap() const { return _emissiveMap ? _emissiveMap : emissiveTexture(); }
        void setEmissiveMap(Texture* texture)
        {
            _emissiveMap = texture;
            setEmissiveTexture(texture);
            setHasEmissiveTexture(texture != nullptr);
            markUniformsDirty();
        }
        // --- Vertex color routing ---
        // A mesh's vertex colors modulate the diffuse lane by default.
        // Route them to emissive instead for additive stamps like decals, where the
        // color has to survive an unlit, black-diffuse material.
        bool diffuseVertexColor() const { return _diffuseVertexColor; }
        void setDiffuseVertexColor(const bool value) { _diffuseVertexColor = value; markUniformsDirty(); }
        bool emissiveVertexColor() const { return _emissiveVertexColor; }
        void setEmissiveVertexColor(const bool value) { _emissiveVertexColor = value; markUniformsDirty(); }
        /// Whether the mesh's vertex colours are gamma encoded. Vertex colours are
        /// LINEAR by default (glTF's COLOR_0 is); set this for colours authored in
        /// gamma space, and the vertex stage decodes them (pow 2.2) before the
        /// fragment stage multiplies them into the linear base or emissive colour.
        /// A shader variant (VT_FEATURE_VERTEX_COLOR_GAMMA).
        bool vertexColorGamma() const { return _vertexColorGamma; }
        void setVertexColorGamma(const bool value) { _vertexColorGamma = value; markUniformsDirty(); }
        // --- Normal ---
        // Writes through to the base Material's slot, as setAoMap does: a glTF
        // material binds the BASE slot, so a StandardMaterial-only store made
        // `setNormalMap(nullptr)` clear nothing on a loaded material.
        Texture* normalMap() const { return _normalMap ? _normalMap : normalTexture(); }
        void setNormalMap(Texture* texture)
        {
            _normalMap = texture;
            setNormalTexture(texture);
            setHasNormalTexture(texture != nullptr);
            markUniformsDirty();
        }
        float bumpiness() const { return _bumpiness; }
        void setBumpiness(const float value) { _bumpiness = value; markUniformsDirty(); }
        // --- Opacity ---
        float opacity() const { return _opacity; }
        void setOpacity(const float value) { _opacity = value; markUniformsDirty(); }
        Texture* opacityMap() const { return _opacityMap; }
        void setOpacityMap(Texture* texture) { _opacityMap = texture; markUniformsDirty(); }
        /// Channel of the opacity map that supplies opacity (default "a").
        MapChannel opacityMapChannel() const { return _opacityMapChannel; }
        void setOpacityMapChannel(const MapChannel value) { _opacityMapChannel = value; markUniformsDirty(); }
        // --- Height / Parallax ---
        Texture* heightMap() const { return _heightMap; }
        void setHeightMap(Texture* texture) { _heightMap = texture; markUniformsDirty(); }
        /** Baked lightmap sampled at UV1 and added to indirect diffuse. */
        Texture* lightMap() const { return _lightMap; }
        void setLightMap(Texture* texture) { _lightMap = texture; markUniformsDirty(); }
        float heightMapFactor() const { return _heightMapFactor; }
        void setHeightMapFactor(const float value) { _heightMapFactor = value; markUniformsDirty(); }
        /**
         * The height-map value that sits at the level of the geometry. Texels above
         * it stand proud of the polygon, texels below sink into it. 1 treats the map
         * as pure depth carved below the surface; the default 0.5 pivots the relief
         * around mid-grey.
         */
        float heightMapBase() const { return _heightMapBase; }
        void setHeightMapBase(const float value) { _heightMapBase = value; markUniformsDirty(); }
        /**
         * Parallax self-shadowing strength, 0..1. Above 0 the directional light
         * marches the height field and darkens texels its ray passes over. Costs a
         * second march per shaded fragment, so it is off by default.
         */
        float heightMapShadow() const { return _heightMapShadow; }
        void setHeightMapShadow(const float value) { _heightMapShadow = value; markUniformsDirty(); }
        // --- Anisotropy ---
        float anisotropy() const { return _anisotropy; }
        void setAnisotropy(const float value) { _anisotropy = value; markUniformsDirty(); }
        /// In DEGREES: turns the anisotropy direction from
        /// the tangent toward the bitangent. A negative `anisotropy` (a
        /// deprecated form) adds 90 on top.
        float anisotropyRotation() const { return _anisotropyRotation; }
        void setAnisotropyRotation(const float degrees) { _anisotropyRotation = degrees; markUniformsDirty(); }
        // --- Transmission / Refraction ---
        float transmissionFactor() const { return _transmissionFactor; }
        void setTransmissionFactor(const float value) { _transmissionFactor = value; markUniformsDirty(); }
        // DEVIATION: an index of refraction (default 1.5). Upstream stores the ratio
        // eta = 1 / IOR (default 1 / 1.5) and hands it straight to refract(); this
        // port converts in the shader, and KHR_materials_ior loads without inverting.
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
        // The AO map and the base Material's occlusion texture are ONE slot (4) and one
        // shader feature; the GLB parser fills the base property, other code and the
        // examples talk to aoMap. Keep the two in step here, or `setAoMap(nullptr)` on a
        // loaded material clears nothing.
        Texture* aoMap() const { return _aoMap ? _aoMap : occlusionTexture(); }
        void setAoMap(Texture* texture)
        {
            _aoMap = texture;
            setOcclusionTexture(texture);
            setHasOcclusionTexture(texture != nullptr);
            markUniformsDirty();
        }
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
        const Vector2& glossMapTiling() const { return _glossMapTiling; }
        void setGlossMapTiling(const Vector2& v) { _glossMapTiling = v; markUniformsDirty(); }
        const Vector2& glossMapOffset() const { return _glossMapOffset; }
        void setGlossMapOffset(const Vector2& v) { _glossMapOffset = v; markUniformsDirty(); }
        float glossMapRotation() const { return _glossMapRotation; }
        void setGlossMapRotation(float deg) { _glossMapRotation = deg; markUniformsDirty(); }
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
        const Vector2& opacityMapTiling() const { return _opacityMapTiling; }
        void setOpacityMapTiling(const Vector2& v) { _opacityMapTiling = v; markUniformsDirty(); }
        const Vector2& opacityMapOffset() const { return _opacityMapOffset; }
        void setOpacityMapOffset(const Vector2& v) { _opacityMapOffset = v; markUniformsDirty(); }
        float opacityMapRotation() const { return _opacityMapRotation; }
        void setOpacityMapRotation(float deg) { _opacityMapRotation = deg; markUniformsDirty(); }
        // --- Rendering flags ---
        bool useFog() const { return _useFog; }
        void setUseFog(const bool value) { _useFog = value; markUniformsDirty(); }
        /// Whether the scene's tone mapping and exposure apply to this material. Off, it
        /// renders as TONEMAP_NONE (neither curve nor exposure) while a gamma target still
        /// gets its gamma encode; UI text and images turn it off. A shader variant
        /// (VT_FEATURE_NO_TONEMAP), so the setter marks the uniforms dirty.
        bool useTonemap() const { return _useTonemap; }
        void setUseTonemap(const bool value) { _useTonemap = value; markUniformsDirty(); }
        /// Whether scene lights shade this material. Off, no light reaches it — neither
        /// the light list nor the clustered lights — while ambient, environment
        /// reflections, fog and the layer combine still run (VT_FEATURE_NO_LIGHTS). An
        /// emissive light-source shape with a black diffuse therefore draws as its
        /// emission alone. For a flat colour with no shading at all, see setUnlit.
        bool useLighting() const { return _useLighting; }
        void setUseLighting(const bool value) { _useLighting = value; markUniformsDirty(); }
        /// DEVIATION: a fully unlit output — the diffuse colour plus the emission, with
        /// no ambient, reflection or fog (VT_FEATURE_UNLIT, the path KHR_materials_unlit
        /// also takes). It carries the engine's own overlays: UI elements and MSDF text,
        /// outlines, the view cube. Upstream reaches the same pixels through useLighting
        /// off plus a black diffuse, no skybox and no fog; MSDF text is drawn only by
        /// this path, so an MSDF map turns it on by itself.
        bool unlit() const { return _unlit; }
        void setUnlit(const bool value) { _unlit = value; markUniformsDirty(); }
        bool useSkybox() const { return _useSkybox; }
        void setUseSkybox(const bool value) { _useSkybox = value; markUniformsDirty(); }
        bool twoSidedLighting() const { return _twoSidedLighting; }
        void setTwoSidedLighting(const bool value) { _twoSidedLighting = value; markUniformsDirty(); }
        // --- Planar Reflection ---
        // DEVIATION: planar reflection is handled at the application level as a script.
        // We promote it to a material property for simpler integration with the shader variant system.
        Texture* reflectionMap() const { return _reflectionMap; }
        void setReflectionMap(Texture* texture) { _reflectionMap = texture; markUniformsDirty(); }
        // --- Clearcoat ---
        // dual-layer clearcoat material (KHR_materials_clearcoat).
        // A thin dielectric coat (IOR 1.5, F0=0.04) over the standard PBR base.
        float clearCoat() const { return _clearCoat; }
        void setClearCoat(const float value) { _clearCoat = value; markUniformsDirty(); }
        float clearCoatGloss() const { return _clearCoatGloss; }
        void setClearCoatGloss(const float value) { _clearCoatGloss = value; markUniformsDirty(); }
        bool clearCoatGlossInvert() const { return _clearCoatGlossInvert; }
        void setClearCoatGlossInvert(const bool value) { _clearCoatGlossInvert = value; markUniformsDirty(); }
        float clearCoatBumpiness() const { return _clearCoatBumpiness; }
        void setClearCoatBumpiness(const float value) { _clearCoatBumpiness = value; markUniformsDirty(); }
        /// Channel of the clearcoat map that supplies intensity
        /// (default "g"; KHR_materials_clearcoat stores it in R).
        MapChannel clearCoatMapChannel() const { return _clearCoatMapChannel; }
        void setClearCoatMapChannel(const MapChannel value) { _clearCoatMapChannel = value; markUniformsDirty(); }
        /// Channel of the clearcoat gloss map (default "g").
        MapChannel clearCoatGlossMapChannel() const { return _clearCoatGlossMapChannel; }
        void setClearCoatGlossMapChannel(const MapChannel value) { _clearCoatGlossMapChannel = value; markUniformsDirty(); }
        Texture* clearCoatMap() const { return _clearCoatMap; }
        void setClearCoatMap(Texture* texture) { _clearCoatMap = texture; markUniformsDirty(); }
        Texture* clearCoatGlossMap() const { return _clearCoatGlossMap; }
        void setClearCoatGlossMap(Texture* texture) { _clearCoatGlossMap = texture; markUniformsDirty(); }
        Texture* clearCoatNormalMap() const { return _clearCoatNormalMap; }
        void setClearCoatNormalMap(Texture* texture) { _clearCoatNormalMap = texture; markUniformsDirty(); }
        // --- Sheen (KHR_materials_sheen) ---
        // fabric/velvet sheen layer (Charlie sheen BRDF).
        // DEVIATION: sheen is colour + ROUGHNESS and is off while the colour is black.
        // Upstream stores colour + sheenGloss with a separate useSheen flag. Both are
        // off by default, so the defaults agree; the parameterisation does not.
        const Color& sheenColor() const { return _sheenColor; }
        void setSheenColor(const Color& value) { _sheenColor = value; markUniformsDirty(); }
        float sheenRoughness() const { return _sheenRoughness; }
        void setSheenRoughness(const float value) { _sheenRoughness = value; markUniformsDirty(); }
        // --- Iridescence (KHR_materials_iridescence) ---
        // thin-film interference layer.
        float iridescenceIntensity() const { return _iridescenceIntensity; }
        void setIridescenceIntensity(const float value) { _iridescenceIntensity = value; markUniformsDirty(); }
        // DEVIATION: an IOR with KHR_materials_iridescence's default of 1.3. Upstream
        // stores iridescenceRefractionIndex as eta, defaulting to 1 / 1.5.
        float iridescenceIOR() const { return _iridescenceIOR; }
        void setIridescenceIOR(const float value) { _iridescenceIOR = value; markUniformsDirty(); }
        float iridescenceThicknessMax() const { return _iridescenceThicknessMax; }
        void setIridescenceThicknessMax(const float value) { _iridescenceThicknessMax = value; markUniformsDirty(); }
        // --- Spec-Gloss map (KHR_materials_pbrSpecularGlossiness) ---
        // The specular workflow itself is specular() + gloss() with useMetalness
        // false; this map adds rgb = specular colour (sRGB), a = gloss.
        // METAL ONLY: it rides the metal-rough binding (slot 3), and the Vulkan
        // fragment stage has no reinterpretation of that sample.
        Texture* specGlossMap() const { return _specGlossMap; }
        void setSpecGlossMap(Texture* texture) { _specGlossMap = texture; markUniformsDirty(); }
        // --- Detail Normals ---
        // detail normal map overlay blended with primary normal.
        float detailNormalScale() const { return _detailNormalScale; }
        void setDetailNormalScale(const float value) { _detailNormalScale = value; markUniformsDirty(); }
        Texture* detailNormalMap() const { return _detailNormalMap; }
        void setDetailNormalMap(Texture* texture) { _detailNormalMap = texture; markUniformsDirty(); }
        const TextureTransform& detailNormalTransform() const { return _detailNormalTransform; }
        void setDetailNormalTransform(const TextureTransform& t) { _detailNormalTransform = t; markUniformsDirty(); }
        // --- Displacement ---
        // vertex displacement along normals.
        float displacementScale() const { return _displacementScale; }
        void setDisplacementScale(const float value) { _displacementScale = value; markUniformsDirty(); }
        float displacementBias() const { return _displacementBias; }
        void setDisplacementBias(const float value) { _displacementBias = value; markUniformsDirty(); }
        Texture* displacementMap() const { return _displacementMap; }
        void setDisplacementMap(Texture* texture) { _displacementMap = texture; markUniformsDirty(); }
        // --- Oren-Nayar ---
        // roughness-dependent diffuse model (alternative to Lambertian).
        bool useOrenNayar() const { return _useOrenNayar; }
        void setUseOrenNayar(const bool value) { _useOrenNayar = value; markUniformsDirty(); }
        // when true, the material accumulates shadow factors and outputs
        // them via multiplicative blending (LIT_SHADOW_CATCHER shader path).
        bool shadowCatcher() const { return _shadowCatcher; }
        void setShadowCatcher(const bool value) { _shadowCatcher = value; markUniformsDirty(); }
    private:
        // updateUniforms' groups, in the order it packs them; packFlags sets every
        // flag bit StandardMaterial owns.
        void packMapTransforms(MaterialUniforms& uniforms) const;
        void packSurface(MaterialUniforms& uniforms) const;
        void packEmissiveAndAmbient(MaterialUniforms& uniforms) const;
        void packMsdf(MaterialUniforms& uniforms) const;
        void packAnisotropy(MaterialUniforms& uniforms) const;
        /// The non-metal F0 of the metalness workflow and the specular workflow's F0 and gloss.
        void packMetalnessSpecular(MaterialUniforms& uniforms) const;
        void packTransmission(MaterialUniforms& uniforms) const;
        /// Height map, clearcoat, sheen, iridescence, detail normal and displacement.
        void packLayers(MaterialUniforms& uniforms) const;
        void packFlags(MaterialUniforms& uniforms) const;

        Color _diffuse = Color(1.0f, 1.0f, 1.0f, 1.0f);
        Texture* _diffuseMap = nullptr;

        Color _specular = Color(0.0f, 0.0f, 0.0f, 1.0f);

        float _metalness = 1.0f;
        bool _useMetalness = false;
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

        Color _ambient = Color(1.0f, 1.0f, 1.0f, 1.0f);
        Texture* _msdfMap = nullptr;
        float _msdfPxRange = 2.0f;
        float _msdfIntensity = 0.0f;
        Color _msdfOutlineColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float _msdfOutlineThickness = 0.0f;
        Color _msdfShadowColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        Vector2 _msdfShadowOffset = Vector2(0.0f, 0.0f);

        Color _emissive = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float _emissiveIntensity = 1.0f;
        bool _diffuseVertexColor = true;
        bool _emissiveVertexColor = false;
        Texture* _emissiveMap = nullptr;

        Texture* _normalMap = nullptr;
        float _bumpiness = 1.0f;

        float _opacity = 1.0f;
        Texture* _opacityMap = nullptr;
        MapChannel _opacityMapChannel = MapChannel::MAP_CHANNEL_A;

        Texture* _heightMap = nullptr;
        Texture* _lightMap = nullptr;
        float _heightMapFactor = 1.0f;
        float _heightMapBase = 0.5f;
        float _heightMapShadow = 0.0f;

        float _anisotropy = 0.0f;
        float _anisotropyRotation = 0.0f;
        bool _useMetalnessSpecularColor = false;
        float _specularityFactor = 1.0f;

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
        Vector2 _glossMapTiling{1.0f, 1.0f};
        Vector2 _glossMapOffset{0.0f, 0.0f};
        float _glossMapRotation = 0.0f;

        Vector2 _aoMapTiling{1.0f, 1.0f};
        Vector2 _aoMapOffset{0.0f, 0.0f};
        float _aoMapRotation = 0.0f;

        Vector2 _emissiveMapTiling{1.0f, 1.0f};
        Vector2 _emissiveMapOffset{0.0f, 0.0f};
        float _emissiveMapRotation = 0.0f;

        Vector2 _opacityMapTiling{1.0f, 1.0f};
        Vector2 _opacityMapOffset{0.0f, 0.0f};
        float _opacityMapRotation = 0.0f;

        Texture* _reflectionMap = nullptr;

        // clearcoat properties.
        float _clearCoat = 0.0f;
        float _clearCoatGloss = 1.0f;
        bool _clearCoatGlossInvert = false;
        float _clearCoatBumpiness = 1.0f;
        Texture* _clearCoatMap = nullptr;
        Texture* _clearCoatGlossMap = nullptr;
        Texture* _clearCoatNormalMap = nullptr;
        MapChannel _clearCoatMapChannel = MapChannel::MAP_CHANNEL_G;
        MapChannel _clearCoatGlossMapChannel = MapChannel::MAP_CHANNEL_G;

        // sheen properties (KHR_materials_sheen).
        Color _sheenColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float _sheenRoughness = 0.0f;

        // iridescence properties (KHR_materials_iridescence).
        float _iridescenceIntensity = 0.0f;
        float _iridescenceIOR = 1.3f;
        float _iridescenceThicknessMax = 0.0f;

        // spec-gloss properties (KHR_materials_pbrSpecularGlossiness).
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
        bool _useTonemap = true;
        bool _useLighting = true;
        bool _unlit = false;
        bool _vertexColorGamma = false;
        bool _useSkybox = true;
        bool _twoSidedLighting = false;
        bool _shadowCatcher = false;

    };
}
