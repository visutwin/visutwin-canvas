// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The GPU instance-cull kernels' dispatch size (the kernels are engine/shaders/slang/
// programs/instance-cull-reset.slang and instance-cull.slang, run over the generic Compute
// seam by ComputeInstanceCuller). Two dispatches a cull: RESET (one thread) writes the mesh's index
// range into the indirect draw arguments and zeroes their instance count; CULL (one thread
// per instance) tests each instance's bounding sphere against six planes and appends the
// visible ones to the output with an atomic add on that count, which the indirect draw
// then reads as its instance count.
//
// Bindings follow Compute's name-order contract: buffers "args", "input", "output" at
// 0, 1, 2 and the uniform block (InstanceCullParams, 128 bytes) after them, buffer(3) on
// Metal and binding 3 on Vulkan. An instance is the 80-byte InstanceData record (model
// matrix + colour); the draw arguments are the 20-byte indexed-indirect layout both APIs
// share (index count, instance count, first index, vertex offset, first instance).
//
#pragma once

#include <cstdint>

namespace visutwin::canvas::instance_cull_shaders
{
    constexpr uint32_t kCullThreads = 64u;

}
