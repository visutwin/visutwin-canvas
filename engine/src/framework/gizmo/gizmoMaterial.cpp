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

namespace visutwin::canvas
{
    namespace
    {
        // engine/shaders/slang/programs/gizmo.slang, cached on the device.
        std::shared_ptr<Shader> sharedGizmoShader(const std::shared_ptr<GraphicsDevice>& device)
        {
            return getOrCreateSlangShader(device.get(), "gizmo");
        }
    }

    GizmoMaterial::GizmoMaterial(const std::shared_ptr<GraphicsDevice>& device)
    {
        setName("gizmo-unlit");
        // blendType BLEND_NORMAL, depth test and write left at their defaults,
        // back faces culled (the plane shape turns culling off).
        setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        setDepthState(std::make_shared<DepthState>());
        setCullMode(CullMode::CULLFACE_BACK);
        setTransparent(true);
        if (device) {
            setShaderOverride(sharedGizmoShader(device));
        }
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

    const void* GizmoMaterial::customUniformData(size_t& outSize) const
    {
        outSize = sizeof(_block);
        return &_block;
    }
}
