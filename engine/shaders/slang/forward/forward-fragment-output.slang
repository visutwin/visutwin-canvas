// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// What a forward fragment writes: an application replaces this file with a chunk override
// (Material::setShaderChunk("forward-fragment-output", source)) to write something else. `color`
// is the fragment's colour as the forward program made it, written to the first attachment;
// `outputs` holds the rest, read only where the variant has somewhere to put them: color1-3 for a
// target with more colour attachments (one entry per attachment count), `secondary` for a
// material whose blend state reads the second source colour (BLENDMODE_SRC1_*). The override may
// read vtOutputNormal and vtOutputGloss (the lit path's shading normal and gloss).
//
// By default every attachment gets the colour, so no attachment is left undefined, and the
// second source is zero.
void forwardFragmentOutput(inout float4 color, inout ForwardFragmentOutputs outputs)
{
    outputs.color1 = color;
    outputs.color2 = color;
    outputs.color3 = color;
    outputs.secondary = float4(0.0, 0.0, 0.0, 0.0);
}
