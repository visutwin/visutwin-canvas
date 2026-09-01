// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/vector4.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    class Shader;
    class Texture;

    /**
     * @brief Draws a fullscreen quad with a shader, its input textures and one
     * uniform block (upstream `QuadRender`).
     * @ingroup group_scene_graphics
     *
     * This is the backend-agnostic vehicle for fullscreen effects: an effect is
     * a shader (authored per language), up to 8 input textures and a uniform
     * struct, and needs no per-backend pass class. Textures land on fragment
     * slots 0..7 on both backends; the uniform block rides the per-draw material
     * slot (see `GraphicsDevice::setQuadUniformData`).
     */
    class QuadRender
    {
    public:
        explicit QuadRender(const std::shared_ptr<Shader>& shader);
        ~QuadRender() = default;

        /// Input texture for fragment slot `slot` (0..7).
        void setTexture(size_t slot, Texture* texture);

        /// Uniform block for the draw. Bytes are copied.
        void setUniformData(const void* data, size_t size);
        template <typename T>
        void setUniforms(const T& block)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                "quad uniform blocks are memcpy'd to the GPU");
            setUniformData(&block, sizeof(T));
        }

        void render(const Vector4* viewport = nullptr, const Vector4* scissor = nullptr) const;

    private:
        std::shared_ptr<Shader> _shader;
        std::array<Texture*, 8> _textures{};
        std::vector<uint8_t> _uniformData;
    };
}
