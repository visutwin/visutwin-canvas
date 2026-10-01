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
        // the entity and its whole ancestry (Component::active() combines the two
        // halves). Testing the component's own enabled() flag would leave a script on
        // a disabled entity running every frame.
        const auto* component = _entity ? _entity->script() : nullptr;
        return _enabled && component != nullptr && component->active();
    }

    void Script::setEnabled(const bool value)
    {
        _enabled = value;
        auto* component = _entity ? _entity->script() : nullptr;
        syncState(component != nullptr && component->active());
        if (component && enabled()) {
            component->initializeScriptInstance(this);
        }
    }

    void Script::syncState(const bool componentActive)
    {
        const bool now = _enabled && componentActive;
        if (now == _enabledOld) {
            return;
        }
        _enabledOld = now;
        fire(now ? "enable" : "disable");
        fire("state", now);
    }
}
