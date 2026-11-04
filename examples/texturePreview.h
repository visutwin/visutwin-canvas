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

        /// The texture shown, for one recreated on a resize.
        void setTexture(Texture* texture) { _texture = texture; }

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
            // A single-channel texture (a depth buffer, a mask) shows its channel as grey: what a
            // backend returns in the channels it lacks differs.
            const PixelFormat format = _texture ? _texture->format() : PixelFormat::PIXELFORMAT_RGBA8;
            const bool single = format == PixelFormat::PIXELFORMAT_DEPTH || format == PixelFormat::PIXELFORMAT_DEPTH16 ||
                format == PixelFormat::PIXELFORMAT_DEPTHSTENCIL || format == PixelFormat::PIXELFORMAT_R32F ||
                format == PixelFormat::PIXELFORMAT_R8;
            const float uniforms[4] = {encode, single ? 1.0f : 0.0f, 0.0f, 0.0f};
            setQuadUniformData(uniforms, sizeof(uniforms));
            setQuadTextureBinding(0, _texture);
            RenderPassShaderQuad::execute();
        }

    private:
        static constexpr const char* kSource = R"slang(
import vtquad;
import vtsrgb;
#include "bindings.slang"

struct PreviewUniforms
{
    float4 params;  // x: 1 to gamma-encode linear colour, y: 1 to show the red channel as grey
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
    const float4 texel = previewTexture.SampleLevel(input.uv, 0.0);
    const float3 color = u.params.y > 0.5 ? texel.rrr : texel.rgb;
    const float3 shown = u.params.x > 0.5 ? linearToSrgb(saturate(color)) : color;
    return float4(shown, 1.0);
}
)slang";

        Texture* _texture;
    };
}
