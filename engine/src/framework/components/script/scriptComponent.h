// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.09.2025.
//
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <framework/script/script.h>
#include "framework/components/component.h"

namespace visutwin::canvas
{
    struct ScriptComponentData
    {
    };

    struct ScriptCreateOptions
    {
        bool enabled = true;
        bool preloading = false;
    };

    class ScriptComponent : public Component
    {
    public:
        ScriptComponent(IComponentSystem* system, Entity* entity) : Component(system, entity) {}
        ~ScriptComponent() override;

        Script* create(const std::string& name, const ScriptCreateOptions& options = {});

        template<typename T>
        T* create() {
            static_assert(std::is_base_of_v<Script, T>, "T must derive from Script");
            return static_cast<T*>(create(T::scriptName()));
        }

        /// The script of that name on this component, or null (upstream `get`).
        Script* get(const std::string& name) const
        {
            const auto it = _scriptsIndex.find(name);
            return it != _scriptsIndex.end() ? _scripts[it->second].instance.get() : nullptr;
        }

        void initializeComponentData() override {};
        void cloneFrom(const Component* source) override;
        void resolveClonedReferences(const Component* source, const CloneNodeMap& map) override;

        /// Initializes any script created while this component was inactive.
        /// Fires for both halves of "active": the component's own flag and the
        /// entity's hierarchy state.
        void onEnable() override;
        /// Run initialize() on every script that has not had it, and nothing else.
        /// Separate from postInitialize because the whole point of the second phase
        /// is that it runs after EVERY script in the application has initialized —
        /// running the pair back to back per script, which is what happens for a
        /// script created after start(), lets one script's postInitialize observe a
        /// sibling that has not initialized yet.
        void initializeScripts();

        /// Run postInitialize() on every script that has initialized and not had it.
        void postInitializeScripts();

        void fixedUpdateScripts(float fixedDt);
        void updateScripts(float dt);
        void postUpdateScripts(float dt);

        int executionOrder() const { return _executionOrder; }
        /// Moves the component in its system's update order. The order is read when the
        /// component is inserted, so the setter asks the system to re-sort; it used to
        /// store the value and change nothing.
        void setExecutionOrder(int value);

    private:
        friend class ScriptComponentSystem;   // sets the creation-order default

        void initializeScriptInstance(Script* script);

        // Calls `call` on each script in order, safely against what a script may do
        // from inside its own method: create another script (it is appended, so the
        // walk is by index and it runs in the same pass) or destroy its own entity,
        // which destroys this component in the middle of the loop. See RunState.
        template <typename Call>
        void forEachScript(Call&& call);

        struct ScriptEntry
        {
            std::string name;
            std::unique_ptr<Script> instance;
        };

        // Outlives the component while a loop is running over it. The destructor
        // clears `alive` and, when a loop is in progress, hands the scripts to
        // `retired` instead of freeing them — the script whose method destroyed the
        // entity is still executing — and they are freed when the last loop lets go
        // of this state, after that method has returned.
        struct RunState
        {
            int depth = 0;
            bool alive = true;
            std::vector<std::unique_ptr<Script>> retired;
        };

        std::unordered_map<std::string, size_t> _scriptsIndex;

        std::vector<ScriptEntry> _scripts;
        std::shared_ptr<RunState> _run = std::make_shared<RunState>();
        int _executionOrder = 0;
    };
}
