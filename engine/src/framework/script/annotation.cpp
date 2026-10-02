// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
//
#include "annotation.h"

#include "framework/engine.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    void Annotation::postInitialize()
    {
        Engine* engine = entity() ? entity()->engine() : nullptr;
        if (!engine) {
            return;
        }

        // Notify any listeners that this annotation has been created
        engine->fire("annotation:add", this);

        // Clean up on destroy. The script outlives neither its entity nor the engine,
        // and `destroy` fires while both are still alive.
        once("destroy", [this, engine]() {
            engine->fire("annotation:remove", this);
        });
    }
}
