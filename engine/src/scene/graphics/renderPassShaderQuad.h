// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#pragma once

#include <cstdint>
#include <string>
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
#include "platform/graphics/shaderBindings.h"
#include "platform/graphics/shaderFeatures.h"
#include "platform/graphics/stencilParameters.h"

namespace visutwin::canvas
{
    class QuadRender;
    class VertexBuffer;
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
        /// See GraphicsDevice::setQuadTextureOwnSampler.
        void setQuadTextureOwnSampler(const size_t slot, const bool value)
        {
            if (slot < _quadTextureOwnSampler.size()) {
                _quadTextureOwnSampler[slot] = value;
            }
        }

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

        /// Draws `vertexCount` vertices of `vertexBuffer` (a triangle list) instead of the
        /// fullscreen triangle; see QuadRender::setGeometry. Null for the fullscreen triangle.
        void setGeometry(const std::shared_ptr<VertexBuffer>& vertexBuffer, int vertexCount)
        {
            _geometry = vertexBuffer;
            _geometryVertexCount = vertexCount;
        }

        CullMode cullMode() const { return _cullMode; }

    protected:
        /// Takes the Slang program `program` / `variant` (engine/shaders/slang/programs),
        /// cached on the device, as the pass's shader: what an engine quad pass's
        /// prepareShaders() calls.
        void useSlangShader(const std::string& program, const std::string& variant = {},
            const ShaderFeatureSet& features = {});

        /// Takes the shader compiled at run time from the Slang `source`
        /// (getOrCreateSlangShaderFromSource), cached on the device: an application's own quad
        /// pass, written once for both backends as an engine quad program is (the vtquad module,
        /// VT_DECLARE_QUAD_UNIFORMS and the VT_DECLARE_QUAD_*INPUTn macros).
        void useSlangSource(const std::string& name, const std::string& source, const std::string& variant = {});

        /// Takes the device-cached shader of this name as the pass's shader, creating
        /// it on first use from the source in the device's language: the route for a
        /// hand-written MSL + GLSL quad shader.
        void useCachedShader(const char* cacheKey, const char* vertexEntry, const char* fragmentEntry,
            const char* msl, const char* glsl);

    public:
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
        std::shared_ptr<BlendState> _blendState = BlendState::noBlend();
        // Fullscreen quads composite in 2D, so depth test and write are OFF
        // (DepthState::noDepth). With the default depth state, quads
        // targeting the back buffer would test against UNDEFINED depth: the previous
        // back-buffer pass stores depth with StoreActionDontCare, so LoadActionLoad reads
        // garbage -> frame-random fragment dropouts (flicker).
        std::shared_ptr<DepthState> _depthState = DepthState::noDepth();
        std::shared_ptr<StencilParameters> _stencilFront = nullptr;
        std::shared_ptr<StencilParameters> _stencilBack = nullptr;
        std::optional<Vector4> _viewport;
        std::optional<Vector4> _scissor;

        std::vector<uint8_t> _quadUniformData;
        std::shared_ptr<Shader> _shader = nullptr;
        std::shared_ptr<QuadRender> _quadRender = nullptr;
        std::array<Texture*, kQuadTextureInputs> _quadTextureBindings{};
        std::array<bool, kQuadTextureInputs> _quadTextureOwnSampler{};
        std::shared_ptr<VertexBuffer> _geometry;
        int _geometryVertexCount = 0;
    };
}
