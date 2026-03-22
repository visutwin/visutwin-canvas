// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.09.2025.
//

#pragma once

namespace visutwin::canvas
{
    /**
     * The DynamicBuffers class provides a dynamic memory allocation system for uniform buffer data,
     * particularly for non-persistent uniform buffers. This class utilizes a bump allocator to
     * efficiently allocate aligned memory space from a set of large buffers managed internally. To
     * utilize this system, the user writes data to CPU-accessible staging buffers. When submitting
     * command buffers that require these buffers, the system automatically uploads the data to the GPU
     * buffers. This approach ensures efficient memory management and smooth data transfer between the
     * CPU and GPU.
     */
    class DynamicBuffers
    {
    };
}