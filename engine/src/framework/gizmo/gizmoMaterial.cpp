// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
#include "gizmoMaterial.h"

#include "scene/shader-lib/slangShaders.h"


#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/shaderFeatures.h"

namespace visutwin::canvas
{
    namespace
    {
        // Feature word 0 of the gizmo program (programs/gizmo.slang: kGizmoTexture).
        constexpr uint32_t kGizmoTexture = 1u << 0u;
    }

    GizmoMaterial::GizmoMaterial(const std::shared_ptr<GraphicsDevice>& device)
        : _device(device)
    {
        setName("gizmo-unlit");
        // blendType BLEND_NORMAL, depth test and write left at their defaults,
        // back faces culled (the plane shape turns culling off).
        setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        updateDepthState();
        setCullMode(CullMode::CULLFACE_BACK);
        setTransparent(true);
        updateShader();
    }

    void GizmoMaterial::setColor(const Color& color)
    {
        _color = color;
        _block.color[0] = color.r;
        _block.color[1] = color.g;
        _block.color[2] = color.b;
        _block.color[3] = color.a;
        markUniformsDirty();
    }

    void GizmoMaterial::setDepth(const float depth)
    {
        _depth = depth;
        _block.depth[0] = depth;
        markUniformsDirty();
    }

    void GizmoMaterial::setDepthTest(const bool value)
    {
        _depthTest = value;
        updateDepthState();
    }

    void GizmoMaterial::setDepthWrite(const bool value)
    {
        _depthWrite = value;
        updateDepthState();
    }

    void GizmoMaterial::setDepthBias(const float value)
    {
        _depthBias = value;
        updateDepthState();
    }

    void GizmoMaterial::updateDepthState()
    {
        auto state = std::make_shared<DepthState>();
        state->setDepthTest(_depthTest);
        state->setDepthWrite(_depthWrite);
        state->setDepthBias(_depthBias);
        state->setSlopeDepthBias(_depthBias);
        setDepthState(state);
    }

    void GizmoMaterial::setCoverageTexture(Texture* texture)
    {
        setBaseColorTexture(texture);
        setHasBaseColorTexture(texture != nullptr);
        if (_textured != (texture != nullptr)) {
            _textured = texture != nullptr;
            updateShader();
        }
    }

    void GizmoMaterial::updateShader()
    {
        if (!_device) {
            return;
        }
        ShaderFeatureSet features;
        features.setWord(0, _textured ? kGizmoTexture : 0u);
        setShaderOverride(getOrCreateSlangShader(_device.get(), "gizmo", {}, features));
    }

    const void* GizmoMaterial::customUniformData(size_t& outSize) const
    {
        _drawn = _block;
        _drawn.depth[1] = _device && _device->hdrPass() ? 1.0f : 0.0f;
        outSize = sizeof(_drawn);
        return &_drawn;
    }
}
