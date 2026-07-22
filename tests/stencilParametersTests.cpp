// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#include <iostream>

#include "platform/graphics/stencilParameters.h"

using namespace visutwin::canvas;

int main()
{
    StencilParameters parameters;
    if (parameters.compareFunction() != StencilCompareFunction::Always ||
        parameters.reference() != 0 ||
        parameters.failOperation() != StencilOperation::Keep ||
        parameters.depthFailOperation() != StencilOperation::Keep ||
        parameters.passOperation() != StencilOperation::Keep ||
        parameters.readMask() != 0xff || parameters.writeMask() != 0xff) {
        std::cerr << "FAILED: unexpected default stencil state\n";
        return 1;
    }

    const uint32_t defaultKey = parameters.key();
    const uint32_t defaultStateKey = parameters.stateKey();
    parameters.setCompareFunction(StencilCompareFunction::Equal);
    parameters.setReference(7);
    parameters.setFailOperation(StencilOperation::Zero);
    parameters.setDepthFailOperation(StencilOperation::DecrementClamp);
    parameters.setPassOperation(StencilOperation::Replace);
    parameters.setReadMask(0x0f);
    parameters.setWriteMask(0xf0);

    if (parameters.key() == defaultKey ||
        parameters.stateKey() == defaultStateKey ||
        parameters.compareFunction() != StencilCompareFunction::Equal ||
        parameters.reference() != 7 ||
        parameters.failOperation() != StencilOperation::Zero ||
        parameters.depthFailOperation() != StencilOperation::DecrementClamp ||
        parameters.passOperation() != StencilOperation::Replace ||
        parameters.readMask() != 0x0f || parameters.writeMask() != 0xf0) {
        std::cerr << "FAILED: stencil mutation or cache key is incorrect\n";
        return 1;
    }

    const uint32_t configuredStateKey = parameters.stateKey();
    parameters.setReference(9);
    if (parameters.stateKey() != configuredStateKey) {
        std::cerr << "FAILED: dynamic reference value changed immutable state key\n";
        return 1;
    }

    return 0;
}
