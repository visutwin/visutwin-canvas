// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once

#include <memory>

namespace visutwin::canvas
{
    class Engine;

    /**
     * Handles localization. Responsible for loading localization assets and returning translations for
     * a certain key. Can also handle plural forms.
     */
    class I18n
    {
    public:
        I18n(const std::shared_ptr<Engine>& engine) {}
    };
}
