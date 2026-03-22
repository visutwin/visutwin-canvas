// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025.
//
#pragma once

#include <cstdint>

#include "core/stringIds.h"

namespace visutwin::canvas
{
    /**
     * Holds stencil test settings
     */
    class StencilParameters
    {
    public:
        /**
         * Gets a unique key representing this stencil parameters configuration.
         * Used for fast equality comparison.
         */
        uint32_t key();

    private:
        // Evaluates the key based on current parameters
        void evalKey();

        uint32_t _func;
        uint32_t _ref;
        uint32_t _fail;
        uint32_t _zfail;
        uint32_t _zpass;
        uint32_t _readMask;
        uint32_t _writeMask;

        uint32_t _key = 0;
        bool _dirty = true;

        // Shared string ID cache for generating unique keys
        static StringIds stringIds;
    };
}
