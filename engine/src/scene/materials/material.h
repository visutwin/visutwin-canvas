// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025.
//
#pragma once

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <string>
#include <variant>
#include <vector>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class Texture;

    /**
     * @ingroup group_scene_materials
     * Per-texture UV transform: tiling (scale), offset, and rotation.
     * matches StandardMaterial's per-map tiling/offset/rotation properties.
     * On the GPU side this is pre-computed into a 3×2 affine matrix (two vec3 rows).
     */
    struct TextureTransform
    {
        Vector2 tiling{1.0f, 1.0f};
        Vector2 offset{0.0f, 0.0f};
        float rotation = 0.0f;  // degrees

        bool isIdentity() const
        {
            return tiling.x == 1.0f && tiling.y == 1.0f &&
                   offset.x == 0.0f && offset.y == 0.0f &&
                   rotation == 0.0f;
        }
    };

    enum class AlphaMode
    {
        OPAQUE = 0,
        MASK = 1,
        BLEND = 2
    };

    /**
     * @ingroup group_scene_materials
     * GPU-side material uniform buffer layout. Must match MaterialData in common.metal exactly.
     */
    struct MaterialUniforms
    {
        float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float emissiveColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        uint32_t flags = 0u;
        uint32_t occludeSpecularMode = SPECOCC_AO;
        float alphaCutoff = 0.5f;
        float metallicFactor = 0.0f;
        float roughnessFactor = 1.0f;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        float occludeSpecularIntensity = 1.0f;

        // per-texture UV transforms as pre-computed 3×2 affine matrices.
        // Each pair of float[4] encodes one row of the matrix:
        //   row0 = {cos(θ)*sx, -sin(θ)*sy, ox, 0}
        //   row1 = {sin(θ)*sx,  cos(θ)*sy, 1-sy-oy, 0}
        // Identity: row0={1,0,0,0}, row1={0,1,0,0}
        // GPU applies: uv' = float2(dot(float3(uv,1), row0.xyz), dot(float3(uv,1), row1.xyz))
        float baseColorTransform0[4] = {1, 0, 0, 0};
        float baseColorTransform1[4] = {0, 1, 0, 0};
        float normalTransform0[4]    = {1, 0, 0, 0};
        float normalTransform1[4]    = {0, 1, 0, 0};
        float metalRoughTransform0[4] = {1, 0, 0, 0};
        float metalRoughTransform1[4] = {0, 1, 0, 0};
        float occlusionTransform0[4] = {1, 0, 0, 0};
        float occlusionTransform1[4] = {0, 1, 0, 0};
        float emissiveTransform0[4]  = {1, 0, 0, 0};
        float emissiveTransform1[4]  = {0, 1, 0, 0};

        // clearcoat dual-layer material properties.
        // Ported from StandardMaterial clearCoat/clearCoatGloss/clearCoatBumpiness.
        float clearCoatFactor = 0.0f;       // 0 = disabled, 1 = full clearcoat
        float clearCoatRoughness = 0.0f;    // 0 = mirror, 1 = rough (computed from gloss)
        float clearCoatBumpiness = 1.0f;    // clearcoat normal map intensity
        float heightMapFactor = 0.0f;      // parallax height scale (0 = no parallax)

        float anisotropy = 0.0f;           // anisotropic specular: -1..1 (0 = isotropic)
        float transmissionFactor = 0.0f;   // 0 = opaque, 1 = fully transmissive
        float refractionIndex = 1.5f;      // IOR (1.0 = air, 1.5 = glass, 1.33 = water)
        float thickness = 0.0f;            // volume thickness for absorption scaling

        // --- Sheen (KHR_materials_sheen) ---
        // fabric/velvet sheen layer.
        float sheenColor[4] = {0, 0, 0, 0};   // rgb=sheen color, w=sheen roughness

        // --- Iridescence (KHR_materials_iridescence) ---
        // thin-film interference layer.
        float iridescenceParams[4] = {0, 1.3f, 100.0f, 400.0f}; // intensity, IOR, thicknessMin(nm), thicknessMax(nm)

        // --- Spec-Gloss (KHR_materials_pbrSpecularGlossiness) ---
        // alternative PBR parameterization.
        float specGlossParams[4] = {1, 1, 1, 1};  // rgb=specular color, w=glossiness

        // --- Detail Normals + Displacement ---
        // detail normal overlay and vertex displacement.
        float detailDisplacementParams[4] = {1, 0, 0.5f, 0}; // detailNormalScale, displacementScale, displacementBias, pad

        // --- Detail Normal UV Transform ---
        float detailNormalTransform0[4] = {1, 0, 0, 0};
        float detailNormalTransform1[4] = {0, 1, 0, 0};

        // --- Volume Attenuation (KHR_materials_volume) + Dispersion (KHR_materials_dispersion) ---
        float attenuationParams[4] = {1, 1, 1, 0};  // rgb=attenuationColor, w=attenuationDistance (0 = disabled)
        // x=dispersion strength, y=alphaDither (<0 = unset, dither follows opacity), zw=pad
        float dispersionParams[4] = {0, -1.0f, 0, 0};
    };

    /**
     * Describes a texture that a material wants bound to a specific fragment shader slot.
     */
    struct TextureSlot
    {
        int slot = -1;
        Texture* texture = nullptr;
    };

    /**
     * @brief Base class for GPU materials — owns uniform data, texture bindings, blend/depth state, and shader compilation.
     * @ingroup group_scene_materials
     *
     * A Material determines how a particular MeshInstance is rendered. It specifies
     * a render state including uniforms (MaterialUniforms), textures, shader defines,
     * blend mode, and depth/stencil configuration. This is a base class; use
     * StandardMaterial for PBR surfaces or ShaderMaterial for custom Metal shaders.
     */
    class Material
    {
    public:
        using ParameterValue = std::variant<float, int32_t, uint32_t, bool, Color, Vector2, Vector3, Vector4, Matrix4, Texture*>;

        Material();

        virtual ~Material() = default;

        const std::string& name() const { return _name; }
        void setName(const std::string& name) { _name = name; markUniformsDirty(); }
        bool transparent() const { return _transparent; }
        void setTransparent(const bool value) { _transparent = value; markUniformsDirty(); }
        uint64_t shaderVariantKey() const { return _shaderVariantKey; }
        void setShaderVariantKey(const uint64_t value) { _shaderVariantKey = value; markUniformsDirty(); }
        /**
         * Per-material shader chunk overrides (upstream material.shaderChunks):
         * replace a named chunk's source for programs compiled for THIS material
         * only. Resolution order at composition: material override, then the
         * ProgramLibrary registry override, then the default chunk. The override
         * set's content hash is folded into shader variant cache keys, so edits
         * recompile fresh variants instead of reusing stale binaries.
         */
        void setShaderChunk(const std::string& name, std::string source);
        bool removeShaderChunk(const std::string& name);
        void clearShaderChunks();
        const std::unordered_map<std::string, std::string>& shaderChunkOverrides() const { return _shaderChunkOverrides; }
        uint64_t shaderChunksHash() const { return _shaderChunksHash; }

        const std::shared_ptr<Shader>& shaderOverride() const { return _shaderOverride; }
        void setShaderOverride(const std::shared_ptr<Shader>& shader) { _shaderOverride = shader; markUniformsDirty(); }
        const std::shared_ptr<BlendState>& blendState() const { return _blendState; }
        void setBlendState(const std::shared_ptr<BlendState>& blendState) { _blendState = blendState; markUniformsDirty(); }
        const std::shared_ptr<DepthState>& depthState() const { return _depthState; }
        void setDepthState(const std::shared_ptr<DepthState>& depthState) { _depthState = depthState; markUniformsDirty(); }
        CullMode cullMode() const { return _cullMode; }
        void setCullMode(const CullMode mode) { _cullMode = mode; markUniformsDirty(); }
        const Color& baseColorFactor() const { return _baseColorFactor; }
        void setBaseColorFactor(const Color& value) { _baseColorFactor = value; markUniformsDirty(); }
        Texture* baseColorTexture() const { return _baseColorTexture; }
        void setBaseColorTexture(Texture* texture) { _baseColorTexture = texture; markUniformsDirty(); }
        bool hasBaseColorTexture() const { return _hasBaseColorTexture; }
        void setHasBaseColorTexture(const bool value) { _hasBaseColorTexture = value; markUniformsDirty(); }
        int baseColorUvSet() const { return _baseColorUvSet; }
        void setBaseColorUvSet(const int uvSet) { _baseColorUvSet = uvSet; markUniformsDirty(); }
        Texture* normalTexture() const { return _normalTexture; }
        void setNormalTexture(Texture* texture) { _normalTexture = texture; markUniformsDirty(); }
        float normalScale() const { return _normalScale; }
        void setNormalScale(const float value) { _normalScale = value; markUniformsDirty(); }
        bool hasNormalTexture() const { return _hasNormalTexture; }
        void setHasNormalTexture(const bool value) { _hasNormalTexture = value; markUniformsDirty(); }
        int normalUvSet() const { return _normalUvSet; }
        void setNormalUvSet(const int uvSet) { _normalUvSet = uvSet; markUniformsDirty(); }
        float metallicFactor() const { return _metallicFactor; }
        void setMetallicFactor(const float value) { _metallicFactor = value; markUniformsDirty(); }
        float roughnessFactor() const { return _roughnessFactor; }
        void setRoughnessFactor(const float value) { _roughnessFactor = value; markUniformsDirty(); }
        Texture* metallicRoughnessTexture() const { return _metallicRoughnessTexture; }
        void setMetallicRoughnessTexture(Texture* texture) { _metallicRoughnessTexture = texture; markUniformsDirty(); }
        bool hasMetallicRoughnessTexture() const { return _hasMetallicRoughnessTexture; }
        void setHasMetallicRoughnessTexture(const bool value) { _hasMetallicRoughnessTexture = value; markUniformsDirty(); }
        int metallicRoughnessUvSet() const { return _metallicRoughnessUvSet; }
        void setMetallicRoughnessUvSet(const int uvSet) { _metallicRoughnessUvSet = uvSet; markUniformsDirty(); }
        Texture* occlusionTexture() const { return _occlusionTexture; }
        void setOcclusionTexture(Texture* texture) { _occlusionTexture = texture; markUniformsDirty(); }
        bool hasOcclusionTexture() const { return _hasOcclusionTexture; }
        void setHasOcclusionTexture(const bool value) { _hasOcclusionTexture = value; markUniformsDirty(); }
        int occlusionUvSet() const { return _occlusionUvSet; }
        void setOcclusionUvSet(const int uvSet) { _occlusionUvSet = uvSet; markUniformsDirty(); }
        float occlusionStrength() const { return _occlusionStrength; }
        void setOcclusionStrength(const float value) { _occlusionStrength = value; markUniformsDirty(); }
        bool occludeDirect() const { return _occludeDirect; }
        void setOccludeDirect(const bool value) { _occludeDirect = value; markUniformsDirty(); }
        uint32_t occludeSpecular() const { return _occludeSpecular; }
        void setOccludeSpecular(const uint32_t value) { _occludeSpecular = value; markUniformsDirty(); }
        float occludeSpecularIntensity() const { return _occludeSpecularIntensity; }
        void setOccludeSpecularIntensity(const float value) { _occludeSpecularIntensity = value; markUniformsDirty(); }
        const Color& emissiveFactor() const { return _emissiveFactor; }
        void setEmissiveFactor(const Color& value) { _emissiveFactor = value; markUniformsDirty(); }
        Texture* emissiveTexture() const { return _emissiveTexture; }
        void setEmissiveTexture(Texture* texture) { _emissiveTexture = texture; markUniformsDirty(); }
        bool hasEmissiveTexture() const { return _hasEmissiveTexture; }
        void setHasEmissiveTexture(const bool value) { _hasEmissiveTexture = value; markUniformsDirty(); }
        int emissiveUvSet() const { return _emissiveUvSet; }
        void setEmissiveUvSet(const int uvSet) { _emissiveUvSet = uvSet; markUniformsDirty(); }
        // per-texture UV transforms (tiling, offset, rotation).
        const TextureTransform& baseColorTransform() const { return _baseColorTransform; }
        void setBaseColorTransform(const TextureTransform& t) { _baseColorTransform = t; markUniformsDirty(); }
        const TextureTransform& normalTransform() const { return _normalTransform; }
        void setNormalTransform(const TextureTransform& t) { _normalTransform = t; markUniformsDirty(); }
        const TextureTransform& metalRoughTransform() const { return _metalRoughTransform; }
        void setMetalRoughTransform(const TextureTransform& t) { _metalRoughTransform = t; markUniformsDirty(); }
        const TextureTransform& occlusionTransform() const { return _occlusionTransform; }
        void setOcclusionTransform(const TextureTransform& t) { _occlusionTransform = t; markUniformsDirty(); }
        const TextureTransform& emissiveTransform() const { return _emissiveTransform; }
        void setEmissiveTransform(const TextureTransform& t) { _emissiveTransform = t; markUniformsDirty(); }
        AlphaMode alphaMode() const { return _alphaMode; }
        // Sets the glTF alpha mode and updates the material's BlendState/DepthState/transparent
        // flag to match. BLEND enables standard src-alpha blending, disables depth-write, and
        // marks the material as transparent (rendered in the back-to-front sublayer).
        // OPAQUE/MASK disable blending, re-enable depth-write, and clear the transparent flag.
        // Matches upstream material.blendType setter semantics.
        void setAlphaMode(AlphaMode mode);

        float alphaCutoff() const { return _alphaCutoff; }
        void setAlphaCutoff(const float value) { _alphaCutoff = value; markUniformsDirty(); }
        bool isSkybox() const { return _isSkybox; }
        void setIsSkybox(const bool value) { _isSkybox = value; markUniformsDirty(); }
        void setParameter(const std::string& name, const ParameterValue& value);
        bool removeParameter(const std::string& name);
        void clearParameters();
        const std::unordered_map<std::string, ParameterValue>& parameters() const { return _parameters; }

        /**
         * The packed GPU uniform block, built on demand and reused until something
         * on the material changes. Packing costs a pass over every typed property
         * plus the parameter-override lookups, and the renderer needs it every time
         * a different material is bound — so it is computed on edit, not on bind.
         * Every mutator calls markUniformsDirty(); the debug build re-packs on each
         * call and reports a mismatch, so a mutator that forgets to is caught here
         * rather than as a stale-looking surface much later.
         */
        const MaterialUniforms& packedUniforms() const;

        /// Invalidate the packed uniform cache. Called by every mutator.
        void markUniformsDirty() { _uniformsDirty = true; }
        const ParameterValue* parameter(const std::string& name) const;

        /**
         * Packs material properties into a GPU-ready MaterialUniforms struct.
         * Base implementation reads from typed properties and custom parameter overrides.
         */
        virtual void updateUniforms(MaterialUniforms& uniforms) const;

        /**
         * Re-applies scalar/color setParameter() overrides (material_baseColor,
         * material_metallic, material_roughness, material_normalScale,
         * material_emissive, ...) onto an already-packed uniforms struct.
         * Called at the end of updateUniforms(); subclasses that overwrite those
         * fields afterwards (StandardMaterial) must call it again so parameter
         * overrides stay authoritative.
         */
        void applyParameterOverrides(MaterialUniforms& uniforms) const;

        /**
         * Override to provide custom uniform data with a size different from MaterialUniforms.
         * Returns nullptr by default, in which case updateUniforms() is used.
         * When non-null, the returned data is copied directly to the GPU uniform ring buffer
         * at buffer(3), bypassing the standard MaterialUniforms path.
         *
         * The shader must interpret the raw bytes correctly (e.g., GlobeTileData instead of
         * MaterialData). The ring buffer slot size (based on LightingUniforms) is large enough
         * to accommodate extended uniform structs.
         *
         * @param outSize  Set to the size of the returned data in bytes.
         * @return Pointer to uniform data, or nullptr to use updateUniforms() path.
         */
        virtual const void* customUniformData(size_t& outSize) const { return nullptr; }

        /**
         * Populates the list of textures this material wants bound to fragment shader slots.
         * Slot assignment: 0=baseColor, 1=normal, 3=metalRoughness, 4=occlusion, 5=emissive,
         * 7=clearCoat, 13=clearCoatGloss, 14=clearCoatNormal.
         * Slots 2, 6 are scene-global (envAtlas, shadow) and not material-owned.
         */
        virtual void getTextureSlots(std::vector<TextureSlot>& slots) const;

        uint64_t sortKey() const;

        /**
         * An independent copy of this material (upstream Material::clone).
         *
         * Needed whenever one loaded asset is instantiated more than once and the copies
         * must be configured differently — a container's materials are shared across every
         * instantiateRenderEntity() call, so mutating one otherwise changes them all.
         *
         * The blend and depth state objects are duplicated rather than shared, so the clone
         * is safe to reconfigure. Textures stay shared (non-owning pointers, as everywhere).
         */
        virtual std::shared_ptr<Material> clone() const;

    protected:
        /// Duplicates the state objects a copy-construct would otherwise share. Call from
        /// every clone() override after copy-constructing.
        void detachSharedState();

    public:

    private:
        std::string _name;

        bool _transparent = false;
        uint64_t _shaderVariantKey = 0;
        std::unordered_map<std::string, std::string> _shaderChunkOverrides;
        uint64_t _shaderChunksHash = 0;

        // Optional user-provided shader override.
        std::shared_ptr<Shader> _shaderOverride;

        // Material render states used by the renderer when binding draw calls.
        std::shared_ptr<BlendState> _blendState;
        std::shared_ptr<DepthState> _depthState;
        CullMode _cullMode = CullMode::CULLFACE_BACK;

        // glTF PBR base color subset used by current forward pass.
        Color _baseColorFactor = Color(1.0f, 1.0f, 1.0f, 1.0f);
        Texture* _baseColorTexture = nullptr;
        bool _hasBaseColorTexture = false;
        int _baseColorUvSet = 0;
        Texture* _normalTexture = nullptr;
        bool _hasNormalTexture = false;
        int _normalUvSet = 0;
        float _normalScale = 1.0f;
        float _metallicFactor = 0.0f;
        float _roughnessFactor = 1.0f;
        Texture* _metallicRoughnessTexture = nullptr;
        bool _hasMetallicRoughnessTexture = false;
        int _metallicRoughnessUvSet = 0;
        Texture* _occlusionTexture = nullptr;
        bool _hasOcclusionTexture = false;
        int _occlusionUvSet = 0;
        float _occlusionStrength = 1.0f;
        bool _occludeDirect = false;
        uint32_t _occludeSpecular = SPECOCC_AO;
        float _occludeSpecularIntensity = 1.0f;
        Color _emissiveFactor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        Texture* _emissiveTexture = nullptr;
        bool _hasEmissiveTexture = false;
        int _emissiveUvSet = 0;
        AlphaMode _alphaMode = AlphaMode::OPAQUE;
        float _alphaCutoff = 0.5f;
        bool _isSkybox = false;

        // per-texture UV transforms.
        TextureTransform _baseColorTransform;
        TextureTransform _normalTransform;
        TextureTransform _metalRoughTransform;
        TextureTransform _occlusionTransform;
        TextureTransform _emissiveTransform;

        std::unordered_map<std::string, ParameterValue> _parameters;

        // Packed uniform cache — see packedUniforms(). Mutable so that the const
        // accessor can fill it; updateUniforms() itself writes to the material via
        // const_cast, which is why the dirty flag is cleared AFTER packing.
        mutable MaterialUniforms _cachedUniforms{};
        mutable bool _uniformsDirty = true;
    };

    // Assigns the default material to device cache
    void setDefaultMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<Material>& material);
    std::shared_ptr<Material> getDefaultMaterial(const std::shared_ptr<GraphicsDevice>& device);
}
