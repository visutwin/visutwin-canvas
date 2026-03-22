// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2025.
//
#include <sstream>

#include "stencilParameters.h"

namespace visutwin::canvas
{
    StringIds StencilParameters::stringIds;

    uint32_t StencilParameters::key()
    {
        if (_dirty) {
            evalKey();
        }
        return _key;
    }

    void StencilParameters::evalKey()
    {
        // Create a string key from all parameters
        std::ostringstream oss;
        oss << _func << "," << _ref << "," << _fail << "," << _zfail << "," << _zpass << "," << _readMask << "," << _writeMask;
        std::string key = oss.str();

        // Convert a string to a unique number
        _key = stringIds.get(key);
        _dirty = false;
    }
}