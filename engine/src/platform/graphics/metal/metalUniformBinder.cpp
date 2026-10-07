// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Uniform packing, ring-buffer allocation, and per-pass deduplication.
// Extracted from MetalGraphicsDevice for single-responsibility decomposition.
//
#include "metalUniformBinder.h"

#include "platform/graphics/lightingBlock.h"
#include "platform/graphics/lightingDerivation.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iterator>
#include "metalUniformRingBuffer.h"
#include "metalUtils.h"
#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "core/utils.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "scene/constants.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    using metal::toSimdMatrix;

    namespace
    {
        struct SceneData
        {
            simd::float4x4 projViewMatrix;
        };

        struct ModelData
        {
            simd::float4x4 modelMatrix;
            simd::float4x4 normalMatrix;
        };
    }

    // -----------------------------------------------------------------------
    // Transform uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setTransformUniforms(MTL::RenderCommandEncoder* encoder,
        MetalUniformRingBuffer* transformRing,
        const Matrix4& viewProjection, const Matrix4& model)
    {
        if (!encoder) {
            return;
        }

        // SceneData (VP matrix) at slot 1 — only re-send when the VP changes.
        // Within a single camera/layer pass the VP is identical for all draws,
        // but multi-layer passes may switch cameras with different VP matrices.
        const SceneData sceneData{toSimdMatrix(viewProjection)};
        if (!_sceneDataBoundThisPass ||
            std::memcmp(&sceneData.projViewMatrix, &_cachedSceneVP, sizeof(simd::float4x4)) != 0) {
            encoder->setVertexBytes(&sceneData, sizeof(SceneData), 1);
            // The fragment stage reads it too: a VSM spot shadow pass recovers the
            // light's position and range from it (shadowDistanceRatio).
            encoder->setFragmentBytes(&sceneData, sizeof(SceneData), 1);
            _cachedSceneVP = sceneData.projViewMatrix;
            _sceneDataBoundThisPass = true;
        }

        // ModelData at slot 2 — changes per draw, allocated from ring buffer.
        // Normal matrix = (M^-1)^T of the upper-left 3x3, built by Matrix4::normalMatrix
        // from 3x3 cofactors instead of a full 4x4 inverse().transpose() (~5-7x cheaper).
        // The cofactor matrix C satisfies: (M^-1)^T = C / det(M).
        // Since the vertex shader multiplies by float4(normal, 0.0), only the 3x3 matters.
        //
        // Dividing by the SIGNED determinant is what makes this the inverse
        // transpose rather than a cofactor matrix, and a mirrored mesh depends on
        // that sign: its normals flip with its surface, and the renderer flips
        // which of its faces is culled to match.
        // A near-singular 3x3 (|det| <= 1e-8) uploads a zero normal matrix.
        const ModelData modelData{
            toSimdMatrix(model),
            toSimdMatrix(model.normalMatrix())
        };
        // Allocate ModelData from ring buffer and update offset (slot 2).
        // The ring buffer itself was bound to slot 2 in startRenderPass().
        const size_t transformOffset = transformRing->allocate(&modelData, sizeof(ModelData));
        encoder->setVertexBufferOffset(transformOffset, 2);
    }

    // -----------------------------------------------------------------------
    // Lighting uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setLightingUniforms(const Color& ambientColor,
        const std::vector<GpuLightData>& lights, const Vector3& cameraPosition,
        const bool enableNormalMaps, const float exposure,
        const FogParams& fogParams, const ShadowParams& shadowParams,
        const int toneMapping, const Vector3* ambientSH, const Matrix4* viewProjection,
        const uint32_t meshLightMask)
    {
        // The values are decided in deriveLighting and laid out by packLightingBlock, both
        // shared with the Vulkan backend; this only keeps the textures to bind.
        markLightingChanged();
        auto& lu = _lightingUniforms;
        const DerivedLighting derived = deriveLighting(ambientColor, lights, std::size(lu.lights),
            fogParams, shadowParams, ambientSH, viewProjection, meshLightMask);
        LightingBlockTextures textures;
        packLightingBlock(lu, textures, derived, shadowParams, cameraPosition, exposure, toneMapping,
            enableNormalMaps);

        _shadowTexture = textures.shadowMap[0];
        _shadowTexture1 = textures.shadowMap[1];
        // A VSM spot's map is a colour (moments) texture, which a depth2d argument
        // cannot take: it goes to its own slot (37 / 38) and the depth slot stays empty.
        _localShadowTexture0 = textures.localVsm[0] ? nullptr : textures.localShadowMap[0];
        _localShadowTexture1 = textures.localVsm[1] ? nullptr : textures.localShadowMap[1];
        _localVsmTexture0 = textures.localVsm[0] ? textures.localShadowMap[0] : nullptr;
        _localVsmTexture1 = textures.localVsm[1] ? textures.localShadowMap[1] : nullptr;
        _omniShadowCube0 = textures.omniShadowCube[0];
        _omniShadowCube1 = textures.omniShadowCube[1];
        _cookieTexture2D0 = textures.cookie2D[0];
        _cookieTexture2D1 = textures.cookie2D[1];
        _cookieTextureCube0 = textures.cookieCube[0];
        _cookieTextureCube1 = textures.cookieCube[1];
    }

    // -----------------------------------------------------------------------
    // Environment uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setCameraClipPlanes(const float nearClip, const float farClip)
    {
        _lightingUniforms.cameraNearFar[0] = nearClip;
        _lightingUniforms.cameraNearFar[1] = farClip;
        markLightingChanged();
    }

    void MetalUniformBinder::setDebugShaderPass(const uint32_t mode)
    {
        // flagsAndPad[0] is the bitfield; [1] carries the debug pass mode as a plain value.
        _lightingUniforms.flagsAndPad[1] = mode;
        markLightingChanged();
    }

    void MetalUniformBinder::setReflectionProbeUniforms(Texture* cubemap, const Vector3& boxMin,
        const Vector3& boxMax, const bool boxProjection, const float intensity, const float maxLod)
    {
        _reflectionProbeCubeTexture = cubemap;
        markLightingChanged();
        boxMin.store(_lightingUniforms.reflectionProbeBoxMin);
        boxMax.store(_lightingUniforms.reflectionProbeBoxMax);
        ((boxMin + boxMax) * 0.5f).store(_lightingUniforms.reflectionProbePosition);
        _lightingUniforms.reflectionProbeParams[0] = boxProjection ? 1.0f : 0.0f;
        _lightingUniforms.reflectionProbeParams[1] = intensity;
        _lightingUniforms.reflectionProbeParams[2] = maxLod;
    }

    void MetalUniformBinder::setSkyboxRotation(const Quaternion& rotation)
    {
        const Matrix4 r = Matrix4::trs(Vector3(0.0f), rotation, Vector3(1.0f));
        float columns[12] = {};
        for (int i = 0; i < 3; ++i) {
            const Vector4 c = r.getColumn(i);
            columns[i * 4 + 0] = c.getX();
            columns[i * 4 + 1] = c.getY();
            columns[i * 4 + 2] = c.getZ();
        }
        // w of the first column says "rotated", so an unrotated sky skips the multiply
        // and renders bit-identically.
        const bool identity = columns[0] == 1.0f && columns[5] == 1.0f && columns[10] == 1.0f;
        columns[3] = identity ? 0.0f : 1.0f;
        if (std::memcmp(columns, _lightingUniforms.skyboxRotation, sizeof(columns)) == 0) {
            return;
        }
        std::memcpy(_lightingUniforms.skyboxRotation, columns, sizeof(columns));
        markLightingChanged();
    }

    void MetalUniformBinder::setDitherJitter(const Vector4& jitter)
    {
        const float value[4] = {jitter.getX(), jitter.getY(), jitter.getZ(), jitter.getW()};
        if (std::memcmp(value, _lightingUniforms.ditherJitter, sizeof(value)) == 0) {
            return;
        }
        std::memcpy(_lightingUniforms.ditherJitter, value, sizeof(value));
        markLightingChanged();
    }

    void MetalUniformBinder::setEnvironmentUniforms(Texture* envAtlas, const float skyboxIntensity,
        const float skyboxMip, const Vector3& skyDomeCenter, const bool isDome,
        Texture* skyboxCubeMap)
    {
        _envAtlasTexture = envAtlas;
        _skyboxCubeMapTexture = skyboxCubeMap;
        markLightingChanged();

        // skyParams2: xyz = dome center, w = flags (bit0 cubemap, bit1 dome).
        skyDomeCenter.store(_lightingUniforms.skyParams2);
        _lightingUniforms.skyParams2[3] = static_cast<float>(
            (skyboxCubeMap ? 1u : 0u) | (isDome ? 2u : 0u));

        _lightingUniforms.envParams[0] = skyboxIntensity;
        _lightingUniforms.envParams[1] = envAtlas ? 1.0f : 0.0f;
        if (envAtlas) {
            switch (envAtlas->encoding()) {
            case TextureEncoding::RGBP:
                _lightingUniforms.envParams[2] = static_cast<float>(EnvAtlasEncoding::Rgbp);
                break;
            case TextureEncoding::RGBM:
                _lightingUniforms.envParams[2] = static_cast<float>(EnvAtlasEncoding::Rgbm);
                break;
            default:
                _lightingUniforms.envParams[2] = static_cast<float>(EnvAtlasEncoding::Srgb);
                break;
            }
        }
        _lightingUniforms.envParams[3] = skyboxMip;
    }

    // -----------------------------------------------------------------------
    // Atmosphere uniforms
    // -----------------------------------------------------------------------

    void MetalUniformBinder::setAtmosphereUniforms(const void* data, const size_t size)
    {
        // The six atmosphere vec4s are contiguous in the block and laid out exactly like
        // the Scene's storage, so the caller's block copies straight in. A short block
        // writes only its prefix and leaves the remaining defaults.
        if (!data || size == 0 || size > kLightingBlockAtmosphereBytes) {
            return;
        }
        auto* dst = reinterpret_cast<uint8_t*>(&_lightingUniforms) + kLightingBlockAtmosphereOffset;
        if (std::memcmp(dst, data, size) == 0) {
            return;
        }
        std::memcpy(dst, data, size);
        markLightingChanged();
    }

    // -----------------------------------------------------------------------
    // Per-draw uniform submission with deduplication
    // -----------------------------------------------------------------------

    const void* MetalUniformBinder::sharedDefaultBlockKey()
    {
        static const char key = 0;
        return &key;
    }

    void MetalUniformBinder::submitPerDrawUniforms(MTL::RenderCommandEncoder* encoder,
        MetalUniformRingBuffer* uniformRing,
        const void* materialKey,
        const uint64_t materialVersion,
        const void* uniformData,
        const size_t uniformSize,
        const bool hdrPass)
    {
        // Material-slot block at slot 3 — skip the ring allocation when the block is the
        // one the previous draw uploaded (consecutive draws sharing a material carry
        // identical data), or the default block a material-less draw of this pass already
        // uploaded: every opaque shadow caster and prepass draw carries that same block,
        // and need not take a ring slot of its own.
        //
        // A null key is NOT a cache key: a quad pass has no material and puts its own
        // block in this slot, so every quad draw would compare equal to the last and
        // silently reuse the FIRST draw's uniforms. That is invisible while a pass draws
        // one quad, but a pass drawing a rect list (the environment bakes) would have every
        // draw after the first read the first one's block.
        size_t materialOffset;
        if (materialKey != nullptr && _materialBoundThisPass && materialKey == _lastMaterialKey
            && materialVersion == _lastMaterialVersion) {
            materialOffset = _lastMaterialOffset;
        } else if (materialKey == sharedDefaultBlockKey() && _defaultBlockBoundThisPass) {
            materialOffset = _defaultBlockOffset;
        } else {
            materialOffset = uniformRing->allocate(uniformData, uniformSize);
            if (materialKey == sharedDefaultBlockKey()) {
                _defaultBlockOffset = materialOffset;
                _defaultBlockBoundThisPass = true;
            }
        }
        _lastMaterialKey = materialKey;
        _lastMaterialVersion = materialVersion;
        _lastMaterialOffset = materialOffset;
        _materialBoundThisPass = true;
        // Nothing else moves slot 3's offset within a pass, so an unchanged offset is
        // already on the encoder.
        if (!_materialOffsetSetThisPass || materialOffset != _encoderMaterialOffset) {
            encoder->setFragmentBufferOffset(materialOffset, 3);
            encoder->setVertexBufferOffset(materialOffset, 3);
            _encoderMaterialOffset = materialOffset;
            _materialOffsetSetThisPass = true;
        }

        // Set HDR pass flag (bit 5) — forward shaders check this at runtime
        // to skip tonemapping + gamma when CameraFrame handles them.
        const uint32_t flags = hdrPass
            ? (_lightingUniforms.flagsAndPad[0] | (1u << 5))
            : (_lightingUniforms.flagsAndPad[0] & ~(1u << 5));
        if (flags != _lightingUniforms.flagsAndPad[0]) {
            _lightingUniforms.flagsAndPad[0] = flags;
            markLightingChanged();
        }

        // LightingUniforms at slot 4: reuse the previous upload when the block is
        // unchanged, which is nearly every draw — the renderer sets it once per layer.
        // "Unchanged" is a VERSION every writer of the block bumps, so a draw costs one
        // integer compare. A memcmp (let alone a hash) of the whole ~2.6 KB block per draw
        // is a large share of the frame's CPU at high draw counts, and pure waste in a
        // shadow pass, whose draws never read lighting at all. The block is
        // still compared exactly when the version HAS moved, so a writer that put the
        // same values back (the per-layer setLightingUniforms of a second sublayer)
        // costs one compare rather than an upload.
        if (!_lightingBoundThisPass || _lightingVersion != _lastLightingVersion) {
            if (!_lightingBoundThisPass ||
                std::memcmp(&_lightingUniforms, &_lastLightingUniforms, sizeof(LightingUniforms)) != 0) {
                _lastLightingOffset = uniformRing->allocate(&_lightingUniforms, sizeof(LightingUniforms));
                std::memcpy(&_lastLightingUniforms, &_lightingUniforms, sizeof(LightingUniforms));
                // Slot 4's offset is moved only here, so it needs setting only on upload.
                encoder->setFragmentBufferOffset(_lastLightingOffset, 4);
                _lightingBoundThisPass = true;
            }
            _lastLightingVersion = _lightingVersion;
        } else {
            // A writer of _lightingUniforms that skipped markLightingChanged() shows here.
            assert(std::memcmp(&_lightingUniforms, &_lastLightingUniforms, sizeof(LightingUniforms)) == 0);
        }
    }

    // -----------------------------------------------------------------------
    // Pass lifecycle
    // -----------------------------------------------------------------------

    bool MetalUniformBinder::isMaterialChanged(const Material* mat) const
    {
        // Same key AND version as the block submitPerDrawUniforms last uploaded: a material
        // edited between two draws of one pass (markUniformsDirty moves the version) is
        // packed, uploaded and its textures bound again.
        return !_materialBoundThisPass || static_cast<const void*>(mat) != _lastMaterialKey
            || (mat && mat->uniformsVersion() != _lastMaterialVersion);
    }

    void MetalUniformBinder::resetPassState()
    {
        _sceneDataBoundThisPass = false;
        _lightingBoundThisPass = false;
        _materialBoundThisPass = false;
        _lastMaterialKey = nullptr;
        _defaultBlockBoundThisPass = false;
        _materialOffsetSetThisPass = false;
    }
}
