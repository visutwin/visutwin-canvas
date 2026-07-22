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
    StringIds StencilParameters::stateStringIds;

    uint32_t StencilParameters::key()
    {
        if (_dirty) {
            evalKey();
        }
        return _key;
    }

    uint32_t StencilParameters::stateKey()
    {
        if (_stateDirty) {
            evalStateKey();
        }
        return _stateKey;
    }

    void StencilParameters::evalKey()
    {
        // Create a string key from all parameters
        std::ostringstream oss;
        oss << static_cast<uint32_t>(_func) << "," << _ref << ","
            << static_cast<uint32_t>(_fail) << "," << static_cast<uint32_t>(_zfail) << ","
            << static_cast<uint32_t>(_zpass) << "," << _readMask << "," << _writeMask;
        std::string key = oss.str();

        // Convert a string to a unique number
        _key = stringIds.get(key);
        _dirty = false;
    }

    void StencilParameters::evalStateKey()
    {
        std::ostringstream oss;
        oss << static_cast<uint32_t>(_func) << ","
            << static_cast<uint32_t>(_fail) << "," << static_cast<uint32_t>(_zfail) << ","
            << static_cast<uint32_t>(_zpass) << "," << _readMask << "," << _writeMask;
        _stateKey = stateStringIds.get(oss.str());
        _stateDirty = false;
    }
}
