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

        Script* create(const std::string& name, const ScriptCreateOptions& options = {});

        template<typename T>
        T* create() {
            static_assert(std::is_base_of_v<Script, T>, "T must derive from Script");
            return static_cast<T*>(create(T::scriptName()));
        }

        void initializeComponentData() override {};

        void setEnabled(bool value) override;
        void fixedUpdateScripts(float fixedDt);
        void updateScripts(float dt);
        void postUpdateScripts(float dt);

        int executionOrder() const { return _executionOrder; }
        void setExecutionOrder(const int value) { _executionOrder = value; }

    private:
        void initializeScriptInstance(Script* script);

        struct ScriptEntry
        {
            std::string name;
            std::unique_ptr<Script> instance;
        };

        std::unordered_map<std::string, size_t> _scriptsIndex;

        std::vector<ScriptEntry> _scripts;
        int _executionOrder = 0;
    };
}
