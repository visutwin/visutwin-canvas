// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The forward program: every lit, unlit and sky draw. Its chunks live in
// engine/shaders/slang/forward and are composed here by #include in ProgramLibrary's
// registered order (a chunk override replaces one file through the compiler's virtual file
// system). Variants are the feature words, specialization constants on Vulkan and function
// constants on Metal, so one compiled module serves every material.
import vtdraw;

#include "forward/forward-fragment-head.slang"
#include "forward/common-dither.slang"
#include "forward/common-parallax.slang"
#include "forward/common-shadow-pcss.slang"
#include "forward/common-shadow-vsm.slang"
#include "forward/common-cookie.slang"
#include "forward/common-utils.slang"
#include "forward/common-tonemap.slang"
#include "forward/common-material-flags.slang"
#include "forward/common-ltc.slang"
#include "forward/common-brdf.slang"
#include "forward/common-sheen.slang"
#include "forward/common-iridescence.slang"
#include "forward/common-atmosphere.slang"
#include "forward/forward-fragment-surface.slang"
#include "forward/forward-fragment-lights.slang"
#include "forward/forward-fragment-clustered.slang"
#include "forward/forward-fragment-ambient.slang"
#include "forward/forward-fragment-emissive.slang"
#include "forward/forward-fragment-tail.slang"
#include "forward/forward-vertex.slang"
