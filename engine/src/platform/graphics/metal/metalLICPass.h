// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Surface LIC (Line Integral Convolution) post-processing pass.
//
// Renders flow direction as a dense texture-based visualization by
// convolving noise along screen-space velocity vectors.
//
// Pipeline (single full-screen fragment pass):
//   1. Read velocity texture (RG16Float — screen-space velocity per pixel)
//   2. Read tiled noise texture (R8Unorm — 200×200 white noise)
//   3. For each pixel, trace forward/backward through velocity field,
//      accumulate noise values → grayscale LIC output
//   4. Apply contrast enhancement
//   5. Output as R8Unorm or modulate with scene color
//
// Follows the MetalTaaPass decomposition pattern:
//   - Embedded MSL source as string literal
//   - Full-screen triangle from MetalComposePass
//   - Lazy resource creation
//   - Uniforms via setFragmentBytes
//
// References:
//   - Cabral & Leedom, SIGGRAPH 1993 (original LIC)
//   - Laramee et al. 2003 (image-space LIC)
//   - VTK vtkSurfaceLICInterface (production reference)
//
#pragma once

#include <memory>
#include <Metal/Metal.hpp>

namespace visutwin::canvas
{
    class BlendState;
    class DepthState;
    class MetalBindGroupFormat;
    class MetalComposePass;
    class MetalGraphicsDevice;
    class MetalRenderPipeline;
    class RenderTarget;
    class Shader;
    class Texture;

    /**
     * Manages the Surface LIC post-processing pass.
     * Depends on MetalComposePass for shared full-screen vertex buffer/format.
     */
    class MetalLICPass
    {
    public:
        MetalLICPass(MetalGraphicsDevice* device, MetalComposePass* composePass);
        ~MetalLICPass();

        /// Execute the LIC pass on the active render command encoder.
        ///
        /// @param encoder         Active render command encoder.
        /// @param velocityTexture RG16Float texture with screen-space velocity per pixel.
        /// @param noiseTexture    R8Unorm tiled white noise texture (e.g. 200×200).
        /// @param integrationSteps Number of steps in each direction (total kernel = 2L+1).
        /// @param stepSize        Step size in normalized texture coordinates.
        /// @param animationPhase  Phase offset [0, 1] for animated LIC.
        /// @param contrastLo      Low end of contrast enhancement range.
        /// @param contrastHi      High end of contrast enhancement range.
        /// @param pipeline        Metal render pipeline for state lookup.
        /// @param renderTarget    Render target for pipeline state creation.
        /// @param bindGroupFormats Bind group formats for pipeline compatibility.
        /// @param defaultSampler  Linear/wrap sampler for texture reads.
        /// @param defaultDepthStencilState Fallback depth-stencil state.
        void execute(MTL::RenderCommandEncoder* encoder,
            Texture* velocityTexture,
            Texture* noiseTexture,
            int integrationSteps,
            float stepSize,
            float animationPhase,
            float contrastLo,
            float contrastHi,
            MetalRenderPipeline* pipeline,
            const std::shared_ptr<RenderTarget>& renderTarget,
            const std::vector<std::shared_ptr<MetalBindGroupFormat>>& bindGroupFormats,
            MTL::SamplerState* defaultSampler,
            MTL::DepthStencilState* defaultDepthStencilState);

    private:
        void ensureResources();

        MetalGraphicsDevice* _device;
        MetalComposePass* _composePass;

        std::shared_ptr<Shader> _shader;
        std::shared_ptr<BlendState> _blendState;
        std::shared_ptr<DepthState> _depthState;
        MTL::DepthStencilState* _depthStencilState = nullptr;
    };
}
