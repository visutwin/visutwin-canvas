// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <array>
#include <memory>
#include <optional>

#include "core/math/vector4.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/stencilParameters.h"

namespace visutwin::canvas
{
    class QuadRender;
    class Shader;
    class Texture;

    /**
     * Base render pass for fullscreen shader-quad effects.
     */
    class RenderPassShaderQuad : public RenderPass
    {
    public:
        explicit RenderPassShaderQuad(const std::shared_ptr<GraphicsDevice>& device)
            : RenderPass(device) {}

        void setShader(const std::shared_ptr<Shader>& shader);
        std::shared_ptr<Shader> shader() const { return _shader; }
        void setQuadTextureBinding(const size_t slot, Texture* texture)
        {
            if (slot < _quadTextureBindings.size()) {
                _quadTextureBindings[slot] = texture;
            }
        }
        void clearQuadTextureBindings() { _quadTextureBindings.fill(nullptr); }

        void execute() override;

        CullMode cullMode = CullMode::CULLFACE_NONE;
        // DEVIATION: BlendState::NOBLEND singleton is not implemented yet; default BlendState maps to no-blend.
        std::shared_ptr<BlendState> blendState = std::make_shared<BlendState>();
        // DEVIATION: DepthState::NODEPTH singleton is not implemented yet; default depth state is used.
        std::shared_ptr<DepthState> depthState = std::make_shared<DepthState>();
        std::shared_ptr<StencilParameters> stencilFront = nullptr;
        std::shared_ptr<StencilParameters> stencilBack = nullptr;
        std::optional<Vector4> viewport;
        std::optional<Vector4> scissor;

    private:
        std::shared_ptr<Shader> _shader = nullptr;
        std::shared_ptr<QuadRender> _quadRender = nullptr;
        std::array<Texture*, 8> _quadTextureBindings{};
    };
}
