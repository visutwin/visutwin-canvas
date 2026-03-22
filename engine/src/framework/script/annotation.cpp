// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "annotation.h"

#include "framework/engine.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    void Annotation::activate()
    {
        auto* eng = entity()->engine();
        if (eng) {
            eng->fire("annotation:add", this);
        }
    }
}
