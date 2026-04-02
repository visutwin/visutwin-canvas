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
        void setName(const std::string& name) { _name = name; }

        bool transparent() const { return _transparent; }
        void setTransparent(const bool value) { _transparent = value; }

        uint64_t shaderVariantKey() const { return _shaderVariantKey; }
        void setShaderVariantKey(const uint64_t value) { _shaderVariantKey = value; }

        const std::shared_ptr<Shader>& shaderOverride() const { return _shaderOverride; }
        void setShaderOverride(const std::shared_ptr<Shader>& shader) { _shaderOverride = shader; }

        const std::shared_ptr<BlendState>& blendState() const { return _blendState; }
        void setBlendState(const std::shared_ptr<BlendState>& blendState) { _blendState = blendState; }

        const std::shared_ptr<DepthState>& depthState() const { return _depthState; }
        void setDepthState(const std::shared_ptr<DepthState>& depthState) { _depthState = depthState; }

        CullMode cullMode() const { return _cullMode; }
        void setCullMode(const CullMode mode) { _cullMode = mode; }

        const Color& baseColorFactor() const { return _baseColorFactor; }
        void setBaseColorFactor(const Color& value) { _baseColorFactor = value; }

        Texture* baseColorTexture() const { return _baseColorTexture; }
        void setBaseColorTexture(Texture* texture) { _baseColorTexture = texture; }

        bool hasBaseColorTexture() const { return _hasBaseColorTexture; }
        void setHasBaseColorTexture(const bool value) { _hasBaseColorTexture = value; }
        int baseColorUvSet() const { return _baseColorUvSet; }
        void setBaseColorUvSet(const int uvSet) { _baseColorUvSet = uvSet; }

        Texture* normalTexture() const { return _normalTexture; }
        void setNormalTexture(Texture* texture) { _normalTexture = texture; }
        float normalScale() const { return _normalScale; }
        void setNormalScale(const float value) { _normalScale = value; }

        bool hasNormalTexture() const { return _hasNormalTexture; }
        void setHasNormalTexture(const bool value) { _hasNormalTexture = value; }
        int normalUvSet() const { return _normalUvSet; }
        void setNormalUvSet(const int uvSet) { _normalUvSet = uvSet; }

        float metallicFactor() const { return _metallicFactor; }
        void setMetallicFactor(const float value) { _metallicFactor = value; }

        float roughnessFactor() const { return _roughnessFactor; }
        void setRoughnessFactor(const float value) { _roughnessFactor = value; }

        Texture* metallicRoughnessTexture() const { return _metallicRoughnessTexture; }
        void setMetallicRoughnessTexture(Texture* texture) { _metallicRoughnessTexture = texture; }
        bool hasMetallicRoughnessTexture() const { return _hasMetallicRoughnessTexture; }
        void setHasMetallicRoughnessTexture(const bool value) { _hasMetallicRoughnessTexture = value; }
        int metallicRoughnessUvSet() const { return _metallicRoughnessUvSet; }
        void setMetallicRoughnessUvSet(const int uvSet) { _metallicRoughnessUvSet = uvSet; }

        Texture* occlusionTexture() const { return _occlusionTexture; }
        void setOcclusionTexture(Texture* texture) { _occlusionTexture = texture; }
        bool hasOcclusionTexture() const { return _hasOcclusionTexture; }
        void setHasOcclusionTexture(const bool value) { _hasOcclusionTexture = value; }
        int occlusionUvSet() const { return _occlusionUvSet; }
        void setOcclusionUvSet(const int uvSet) { _occlusionUvSet = uvSet; }
        float occlusionStrength() const { return _occlusionStrength; }
        void setOcclusionStrength(const float value) { _occlusionStrength = value; }
        bool occludeDirect() const { return _occludeDirect; }
        void setOccludeDirect(const bool value) { _occludeDirect = value; }
        uint32_t occludeSpecular() const { return _occludeSpecular; }
        void setOccludeSpecular(const uint32_t value) { _occludeSpecular = value; }
        float occludeSpecularIntensity() const { return _occludeSpecularIntensity; }
        void setOccludeSpecularIntensity(const float value) { _occludeSpecularIntensity = value; }

        const Color& emissiveFactor() const { return _emissiveFactor; }
        void setEmissiveFactor(const Color& value) { _emissiveFactor = value; }
        Texture* emissiveTexture() const { return _emissiveTexture; }
        void setEmissiveTexture(Texture* texture) { _emissiveTexture = texture; }
        bool hasEmissiveTexture() const { return _hasEmissiveTexture; }
        void setHasEmissiveTexture(const bool value) { _hasEmissiveTexture = value; }
        int emissiveUvSet() const { return _emissiveUvSet; }
        void setEmissiveUvSet(const int uvSet) { _emissiveUvSet = uvSet; }

        // per-texture UV transforms (tiling, offset, rotation).
        const TextureTransform& baseColorTransform() const { return _baseColorTransform; }
        void setBaseColorTransform(const TextureTransform& t) { _baseColorTransform = t; }
        const TextureTransform& normalTransform() const { return _normalTransform; }
        void setNormalTransform(const TextureTransform& t) { _normalTransform = t; }
        const TextureTransform& metalRoughTransform() const { return _metalRoughTransform; }
        void setMetalRoughTransform(const TextureTransform& t) { _metalRoughTransform = t; }
        const TextureTransform& occlusionTransform() const { return _occlusionTransform; }
        void setOcclusionTransform(const TextureTransform& t) { _occlusionTransform = t; }
        const TextureTransform& emissiveTransform() const { return _emissiveTransform; }
        void setEmissiveTransform(const TextureTransform& t) { _emissiveTransform = t; }

        AlphaMode alphaMode() const { return _alphaMode; }
        void setAlphaMode(const AlphaMode mode) { _alphaMode = mode; }

        float alphaCutoff() const { return _alphaCutoff; }
        void setAlphaCutoff(const float value) { _alphaCutoff = value; }

        bool isSkybox() const { return _isSkybox; }
        void setIsSkybox(const bool value) { _isSkybox = value; }

        void setParameter(const std::string& name, const ParameterValue& value);
        bool removeParameter(const std::string& name);
        void clearParameters();
        const std::unordered_map<std::string, ParameterValue>& parameters() const { return _parameters; }
        const ParameterValue* parameter(const std::string& name) const;

        /**
         * Packs material properties into a GPU-ready MaterialUniforms struct.
         * Base implementation reads from typed properties and custom parameter overrides.
         */
        virtual void updateUniforms(MaterialUniforms& uniforms) const;

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

    private:
        std::string _name;

        bool _transparent = false;
        uint64_t _shaderVariantKey = 0;

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
    };

    // Assigns the default material to device cache
    void setDefaultMaterial(const std::shared_ptr<GraphicsDevice>& device, const std::shared_ptr<Material>& material);
    std::shared_ptr<Material> getDefaultMaterial(const std::shared_ptr<GraphicsDevice>& device);
}
