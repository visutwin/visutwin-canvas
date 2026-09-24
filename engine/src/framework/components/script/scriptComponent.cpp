// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 02.01.2026
//
#include "scriptComponent.h"
#include <framework/entity.h>
#include "framework/script/scriptRegistry.h"
#include "scriptComponentSystem.h"

namespace visutwin::canvas
{
    ScriptComponent::~ScriptComponent()
    {
        if (auto* scriptSystem = dynamic_cast<ScriptComponentSystem*>(system())) {
            scriptSystem->unregisterComponent(this);
        }
        // Destroyed from inside one of our own loops — a script destroyed its
        // entity. Keep every script alive until that loop has unwound out of the
        // running one; the loop sees `alive` false and touches nothing of ours.
        _run->alive = false;
        if (_run->depth > 0) {
            for (auto& entry : _scripts) {
                _run->retired.push_back(std::move(entry.instance));
            }
        }
    }

    template <typename Call>
    void ScriptComponent::forEachScript(Call&& call)
    {
        // A local copy: it keeps the state alive if `this` is destroyed below.
        const std::shared_ptr<RunState> run = _run;
        ++run->depth;
        for (size_t i = 0;; ++i) {
            // `alive` first: once it is false, `this` is gone and so is _scripts.
            if (!run->alive || i >= _scripts.size()) {
                break;
            }
            if (Script* script = _scripts[i].instance.get()) {
                call(script);
            }
        }
        --run->depth;
    }

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

    void ScriptComponent::onEnable()
    {
        // A script created while this component was inactive — disabled, or on a
        // disabled entity — has not initialized yet. Becoming active is when it
        // does, which is upstream's _checkState.
        //
        // This lives in the lifecycle hook rather than in setEnabled because the
        // component becomes active two ways: its own flag, and its ENTITY's. The
        // hook fires for both; setEnabled saw only the first, so a script on an
        // entity that was enabled later never initialized at all.
        forEachScript([this](Script* script) { initializeScriptInstance(script); });
    }

    void ScriptComponent::initializeScriptInstance(Script* script)
    {
        if (!script) {
            return;
        }

        if (!active() || !script->enabled()) {
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

    void ScriptComponent::initializeScripts()
    {
        if (!active()) {
            return;
        }
        forEachScript([](Script* script) {
            if (!script->enabled() || script->_initialized) {
                return;
            }
            script->_initialized = true;
            script->initialize();
        });
    }

    void ScriptComponent::postInitializeScripts()
    {
        if (!active()) {
            return;
        }
        forEachScript([](Script* script) {
            if (!script->enabled() || !script->_initialized || script->_postInitialized) {
                return;
            }
            script->_postInitialized = true;
            script->postInitialize();
        });
    }

    void ScriptComponent::fixedUpdateScripts(const float fixedDt)
    {
        if (!active()) {
            return;
        }

        forEachScript([fixedDt](Script* script) {
            if (script->_initialized && script->enabled()) {
                script->fixedUpdate(fixedDt);
            }
        });
    }

    void ScriptComponent::updateScripts(const float dt)
    {
        if (!active()) {
            return;
        }

        forEachScript([dt](Script* script) {
            if (script->_initialized && script->enabled()) {
                script->update(dt);
            }
        });
    }

    void ScriptComponent::postUpdateScripts(const float dt)
    {
        if (!active()) {
            return;
        }

        forEachScript([dt](Script* script) {
            if (script->_initialized && script->enabled()) {
                script->postUpdate(dt);
            }
        });
    }
}
