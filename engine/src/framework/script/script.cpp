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
        // Note: entity hierarchy enabled state does not yet drive script execution.
        // Graph hierarchy state is not fully wired, so all scripts remain enabled.
        return _enabled && _entity->script()->enabled();
    }
}
