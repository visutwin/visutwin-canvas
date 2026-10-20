// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 20.10.2026
//
// A texture shown in a rectangle of the screen, the stand-in for upstream's TextureRenderer in the
// examples that preview a texture they render. A pass appended to the frame (Renderer::
// addAppendPass) draws it into its viewport on the back buffer, as upstream's previews show it:
// a texture in a float format holds linear colour and is gamma-encoded for the screen, an 8-bit
// one is shown as stored (upstream decodes it as sRGB and encodes it again).
//
#pragma once

#include <memory>

#include "platform/graphics/constants.h"
#include "platform/graphics/texture.h"
#include "scene/graphics/renderPassShaderQuad.h"

namespace visutwin::canvas
{
    class TexturePreviewPass final : public RenderPassShaderQuad
    {
    public:
        TexturePreviewPass(const std::shared_ptr<GraphicsDevice>& device, Texture* texture)
            : RenderPassShaderQuad(device), _texture(texture)
        {
        }

        void prepareShaders() override
        {
            if (!shader()) {
                useSlangSource("example-texture-preview", kSource);
            }
        }

        void execute() override
        {
            prepareShaders();
            const float encode = _texture && isFloatPixelFormat(_texture->format()) ? 1.0f : 0.0f;
            const float uniforms[4] = {encode, 0.0f, 0.0f, 0.0f};
            setQuadUniformData(uniforms, sizeof(uniforms));
            setQuadTextureBinding(0, _texture);
            RenderPassShaderQuad::execute();
        }

    private:
        static constexpr const char* kSource = R"slang(
import vtquad;
#include "bindings.slang"

struct PreviewUniforms
{
    float4 params;  // x: 1 to gamma-encode linear colour
};
VT_DECLARE_QUAD_UNIFORMS(PreviewUniforms, u)
VT_DECLARE_QUAD_INPUT0(previewTexture)

[shader("vertex")]
QuadVarying previewVertex(QuadVertexIn input)
{
    return quadPassThrough(input);
}

[shader("fragment")]
float4 previewFragment(QuadVarying input) : SV_Target0
{
    const float3 color = previewTexture.SampleLevel(input.uv, 0.0).rgb;
    const float3 shown = u.params.x > 0.5 ? pow(saturate(color), float3(1.0 / 2.2)) : color;
    return float4(shown, 1.0);
}
)slang";

        Texture* _texture;
    };
}
