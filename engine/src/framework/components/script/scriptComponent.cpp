// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 02.01.2026
//
#include "scriptComponent.h"
#include <framework/entity.h>
#include "framework/script/scriptRegistry.h"

namespace visutwin::canvas
{
    Script* ScriptComponent::create(const std::string& name, const ScriptCreateOptions& options)
    {
        // 1. Enforce uniqueness (one instance per type)
        if (_scriptsIndex.contains(name)) {
            return nullptr;
        }

        // 2. Create an instance via registry
        auto* engine = _entity ? _entity->engine() : nullptr;
        if (!engine || !engine->scripts()) {
            return nullptr;
        }

        auto instance = engine->scripts()->create(name);
        if (!instance) {
            return nullptr;
        }

        Script* script = instance.get();

        // 3. Bind ownership context
        script->_entity = _entity;
        script->_enabled = options.enabled;

        // 4. Store the script instance
        _scripts.push_back({ name, std::move(instance) });
        _scriptsIndex[name] = _scripts.size() - 1;

        // 5. Initialize attributes / reflected fields (hook point)
        // initializeAttributes(raw);

        // 6. Initialize lifecycle for non-preloading path only.
        if (!options.preloading) {
            initializeScriptInstance(script);
        }

        return script;
    }

    void ScriptComponent::setEnabled(const bool value)
    {
        const bool oldValue = _enabled;

        // Call base to store value and fire onSetEnabled lifecycle.
        Component::setEnabled(value);

        // Mirror JS behavior: transitioning to enabled should initialize scripts
        // that were created disabled/preloading and now became active.
        if (!oldValue && value) {
            for (auto& entry : _scripts) {
                initializeScriptInstance(entry.instance.get());
            }
        }
    }

    void ScriptComponent::initializeScriptInstance(Script* script)
    {
        if (!script) {
            return;
        }

        if (!_enabled || !script->enabled()) {
            return;
        }

        if (!script->_initialized) {
            script->_initialized = true;
            script->initialize();
        }

        if (!script->_postInitialized) {
            script->_postInitialized = true;
            script->postInitialize();
        }
    }

    void ScriptComponent::fixedUpdateScripts(const float fixedDt)
    {
        if (!_enabled) {
            return;
        }

        for (auto& entry : _scripts) {
            auto* script = entry.instance.get();
            if (!script || !script->_initialized || !script->enabled()) {
                continue;
            }
            script->fixedUpdate(fixedDt);
        }
    }

    void ScriptComponent::updateScripts(const float dt)
    {
        if (!_enabled) {
            return;
        }

        for (auto& entry : _scripts) {
            auto* script = entry.instance.get();
            if (!script || !script->_initialized || !script->enabled()) {
                continue;
            }
            script->update(dt);
        }
    }

    void ScriptComponent::postUpdateScripts(const float dt)
    {
        if (!_enabled) {
            return;
        }

        for (auto& entry : _scripts) {
            auto* script = entry.instance.get();
            if (!script || !script->_initialized || !script->enabled()) {
                continue;
            }
            script->postUpdate(dt);
        }
    }
}
