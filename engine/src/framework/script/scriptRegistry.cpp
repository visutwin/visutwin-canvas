// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#include "scriptRegistry.h"

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    void ScriptRegistry::registerType(const std::string& name, ScriptFactory factory) {
        if (name.empty()) {
            throw std::invalid_argument(
                "ScriptRegistry::registerType: script name cannot be empty"
            );
        }

        if (!factory) {
            throw std::invalid_argument(
                "ScriptRegistry::registerType: factory is null for script '" + name + "'"
            );
        }

        auto [it, inserted] = _types.emplace(name, std::move(factory));

        if (!inserted) {
            throw std::runtime_error(
                "ScriptRegistry::registerType: duplicate script type '" + name + "'"
            );
        }
    }

    std::unique_ptr<Script> ScriptRegistry::create(const std::string& name) const {
        // First check local override, then fall back to global
        if (const auto it = _types.find(name); it != _types.end()) {
            return it->second();
        }

        // Fall back to global factory registry
        if (const auto factory = ScriptFactories::instance().getFactory(name)) {
            return factory();
        }

        spdlog::error("ScriptRegistry::create: unknown script type '{}'", name);
        return nullptr;
    }
}
