// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.02.2026.
//
#pragma once

namespace visutwin::canvas
{
    // Runs a one-time lighting math regression validation for forward shading parity.
    // Returns true when all checks pass.
    bool runLightingValidationSelfTest();
}

