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
        // running one; the loop sees `alive` false and touches nothing of ours, and
        // frees the state (forEachScript) when it is done with it.
        _run->alive = false;
        if (_run->depth > 0) {
            for (auto& entry : _scripts) {
                _run->retired.push_back(std::move(entry.instance));
            }
            (void)_run.release();
        }
    }

    template <typename Call>
    void ScriptComponent::forEachScript(Call&& call)
    {
        // The state by its own address: if `this` is destroyed below, the destructor
        // leaves it to this loop (see RunState).
        RunState* run = _run.get();
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
        if (--run->depth == 0 && !run->alive) {
            delete run;
        }
    }

    template <typename Call>
    void ScriptComponent::runPhase(const uint8_t phase, Call&& call)
    {
        // Before anything else, and without reading the entity: most components have
        // no script in most phases.
        if ((_phases & phase) == 0 || !active()) {
            return;
        }
        forEachScript([this, phase, &call](Script* script) {
            // The script's own flag and the component's active state, which is what
            // Script::enabled() is — re-read for each script, since the one before may
            // have disabled the entity.
            if ((script->_phases & phase) != 0 && script->_initialized && script->_enabled && active()) {
                call(script);
            }
        });
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
        if (const uint8_t gained = script->_phases & ~_phases) {
            _phases |= gained;
            if (auto* scripts = dynamic_cast<ScriptComponentSystem*>(system())) {
                scripts->componentGainedPhases(this, gained);
            }
        }
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

    void ScriptComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const ScriptComponent*>(source);
        if (!src) {
            return;
        }
        // Each script by NAME, in the source's order and with its enabled flag, as
        // upstream's cloneComponent does. The clone is not in a hierarchy yet, so none
        // of them initializes here: that waits for the clone to be enabled, after
        // Script::cloneFrom has copied what the script chooses to share.
        _executionOrder = src->_executionOrder;
        for (const auto& entry : src->_scripts) {
            if (!entry.instance) {
                continue;
            }
            if (Script* script = create(entry.name, {.enabled = entry.instance->_enabled})) {
                script->cloneFrom(*entry.instance);
            }
        }
    }

    void ScriptComponent::resolveClonedReferences(const Component* source, const CloneNodeMap& map)
    {
        const auto* src = dynamic_cast<const ScriptComponent*>(source);
        if (!src) {
            return;
        }
        for (const auto& entry : src->_scripts) {
            const auto it = _scriptsIndex.find(entry.name);
            if (entry.instance && it != _scriptsIndex.end() && _scripts[it->second].instance) {
                _scripts[it->second].instance->resolveClonedReferences(*entry.instance, map);
            }
        }
    }

    void ScriptComponent::setExecutionOrder(const int value)
    {
        if (_executionOrder == value) {
            return;
        }
        _executionOrder = value;
        if (auto* scripts = dynamic_cast<ScriptComponentSystem*>(system())) {
            scripts->sortComponents();
        }
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
        runPhase(Script::PHASE_FIXED_UPDATE, [fixedDt](Script* script) { script->fixedUpdate(fixedDt); });
    }

    void ScriptComponent::updateScripts(const float dt)
    {
        runPhase(Script::PHASE_UPDATE, [dt](Script* script) { script->update(dt); });
    }

    void ScriptComponent::postUpdateScripts(const float dt)
    {
        runPhase(Script::PHASE_POST_UPDATE, [dt](Script* script) { script->postUpdate(dt); });
    }
}
