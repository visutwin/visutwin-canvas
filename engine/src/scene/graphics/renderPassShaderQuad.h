// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

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

        /// Uniform block for the quad shader (see GraphicsDevice::setQuadUniformData).
        /// Bytes are copied; blocks must fit kPerDrawUniformCapacity.
        void setQuadUniformData(const void* data, const size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            _quadUniformData.assign(bytes, bytes + size);
        }
        template <typename T>
        void setQuadUniforms(const T& block)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                "quad uniform blocks are memcpy'd to the GPU");
            setQuadUniformData(&block, sizeof(T));
        }

        void execute() override;

        CullMode cullMode() const { return _cullMode; }
        void setCullMode(const CullMode value) { _cullMode = value; }

        const std::shared_ptr<BlendState>& blendState() const { return _blendState; }
        void setBlendState(const std::shared_ptr<BlendState>& value) { _blendState = value; }

        const std::shared_ptr<DepthState>& depthState() const { return _depthState; }
        void setDepthState(const std::shared_ptr<DepthState>& value) { _depthState = value; }

        const std::shared_ptr<StencilParameters>& stencilFront() const { return _stencilFront; }
        void setStencilFront(const std::shared_ptr<StencilParameters>& value) { _stencilFront = value; }

        const std::shared_ptr<StencilParameters>& stencilBack() const { return _stencilBack; }
        void setStencilBack(const std::shared_ptr<StencilParameters>& value) { _stencilBack = value; }

        const std::optional<Vector4>& viewport() const { return _viewport; }
        void setViewport(const std::optional<Vector4>& value) { _viewport = value; }
        void clearViewport() { _viewport.reset(); }

        const std::optional<Vector4>& scissor() const { return _scissor; }
        void setScissor(const std::optional<Vector4>& value) { _scissor = value; }
        void clearScissor() { _scissor.reset(); }

    private:
        CullMode _cullMode = CullMode::CULLFACE_NONE;
        // DEVIATION: BlendState::NOBLEND singleton is not implemented yet; default BlendState maps to no-blend.
        std::shared_ptr<BlendState> _blendState = std::make_shared<BlendState>();
        // DEVIATION: DepthState::NODEPTH singleton is not implemented yet; default depth state is used.
        // Fullscreen quads composite in 2D — depth test/write default OFF, matching
        // upstream drawQuadWithShader (DepthState.NODEPTH). With the default depth
        // state, quads targeting the back buffer would test against UNDEFINED depth:
        // the previous back-buffer pass stores depth with StoreActionDontCare, so
        // LoadActionLoad reads garbage -> frame-random fragment dropouts (flicker).
        // Fullscreen quads composite in 2D — depth test/write default OFF, matching
        // upstream drawQuadWithShader (DepthState.NODEPTH). With the default depth
        // state, quads targeting the back buffer would test against UNDEFINED depth:
        // the previous back-buffer pass stores depth with StoreActionDontCare, so
        // LoadActionLoad reads garbage -> frame-random fragment dropouts (flicker).
        std::shared_ptr<DepthState> _depthState = []() {
            auto state = std::make_shared<DepthState>();
            state->setDepthTest(false);
            state->setDepthWrite(false);
            return state;
        }();
        std::shared_ptr<StencilParameters> _stencilFront = nullptr;
        std::shared_ptr<StencilParameters> _stencilBack = nullptr;
        std::optional<Vector4> _viewport;
        std::optional<Vector4> _scissor;

        std::vector<uint8_t> _quadUniformData;
        std::shared_ptr<Shader> _shader = nullptr;
        std::shared_ptr<QuadRender> _quadRender = nullptr;
        std::array<Texture*, 8> _quadTextureBindings{};
    };
}
