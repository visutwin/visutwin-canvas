// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2026
//
// The shadow program: the forward vertex family and the shadow pass's two fragment stages
// (forward/shadow-fragment.slang). Built without specialization: every feature word reads
// zero, so the vertex stages take their plain paths and the fragments gate on material flags.
import vtdraw;

// The shared vertex stage writes only what a shadow fragment reads (forwardFinish).
#define VT_SHADOW_PROGRAM 1

#include "forward/forward-fragment-head.slang"
#include "forward/common-dither.slang"
#include "forward/common-material-flags.slang"
#include "forward/forward-vertex.slang"
#include "forward/shadow-fragment.slang"
