// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 28.12.2025
//
#include "script.h"

#include "framework/entity.h"
#include <framework/components/script/scriptComponent.h>

namespace visutwin::canvas
{
    bool Script::enabled() const
    {
        // The script's own flag AND its component's ACTIVE state, which folds in
        // the entity and its whole ancestry. Testing the component's own enabled()
        // flag left a script on a disabled entity running every frame, which is
        // what this used to do — with a comment saying hierarchy state was not
        // wired yet. It is: GraphNode::enabled() answers it and Component::active()
        // combines the two halves.
        const auto* component = _entity ? _entity->script() : nullptr;
        return _enabled && component != nullptr && component->active();
    }
}
