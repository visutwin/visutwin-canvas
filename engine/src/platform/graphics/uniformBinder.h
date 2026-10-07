// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Base interface for uniform packing, ring-buffer allocation, and per-pass deduplication.
// Backend implementations (Metal, Vulkan) provide concrete GPU submission logic.
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "platform/graphics/lightingBlock.h"

namespace visutwin::canvas
{
    class Material;
    class Texture;

    /**
     * Abstract base for uniform binding. Holds the per-pass lighting block (the shared
     * LightingBlock) and per-pass deduplication state.
     * Backend subclasses implement actual GPU buffer submission.
     */
    class UniformBinder
    {
    public:
        virtual ~UniformBinder() = default;

        // ---------------------------------------------------------------
        // The per-pass lighting block: THE layout every backend shares
        // (platform/graphics/lightingBlock.h), filled by packLightingBlock.
        // ---------------------------------------------------------------

        using LightingUniforms = LightingBlock;

        // ---------------------------------------------------------------
        // Per-pass lifecycle
        // ---------------------------------------------------------------

        virtual void resetPassState() = 0;

        // ---------------------------------------------------------------
        // Queries
        // ---------------------------------------------------------------

        [[nodiscard]] virtual bool isMaterialChanged(const Material* mat) const = 0;

        [[nodiscard]] virtual Texture* envAtlasTexture() const = 0;
        [[nodiscard]] virtual Texture* skyboxCubeMapTexture() const = 0;
        [[nodiscard]] virtual Texture* reflectionProbeCubeTexture() const { return nullptr; }
        [[nodiscard]] virtual Texture* shadowTexture() const = 0;
        [[nodiscard]] virtual Texture* localShadowTexture0() const = 0;
        [[nodiscard]] virtual Texture* localShadowTexture1() const = 0;
        [[nodiscard]] virtual Texture* omniShadowCube0() const = 0;
        [[nodiscard]] virtual Texture* omniShadowCube1() const = 0;
        // Light cookies: 2D for spot lights, cubemap for omni. Two slots each.
        [[nodiscard]] virtual Texture* cookieTexture2D0() const { return nullptr; }
        [[nodiscard]] virtual Texture* cookieTexture2D1() const { return nullptr; }
        [[nodiscard]] virtual Texture* cookieTextureCube0() const { return nullptr; }
        [[nodiscard]] virtual Texture* cookieTextureCube1() const { return nullptr; }


        /// Access the packed LightingUniforms struct (for backends to submit to GPU).
        [[nodiscard]] const LightingUniforms& lightingUniforms() const { return _lightingUniforms; }

    protected:
        LightingUniforms _lightingUniforms;
    };

    // The block is memcpy'd to the GPU and must mirror the MSL `LightingData` exactly;
    // the layout itself is locked in lightingBlock.h. A plain aggregate, so alignof is 4:
    // the layout works because every vec4 lands 16-aligned by construction.
    static_assert(std::is_trivially_copyable_v<UniformBinder::LightingUniforms>);
    static_assert(sizeof(UniformBinder::LightingUniforms) % 16 == 0);
    static_assert(offsetof(UniformBinder::LightingUniforms, lights) == 48);
}
