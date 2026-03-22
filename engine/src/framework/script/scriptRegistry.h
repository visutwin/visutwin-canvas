// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.10.2025.
//
#pragma once

#include <functional>
#include <memory>

#include "script.h"

// Helper macros to generate unique names using __LINE__
// Extra level of indirection needed to expand __LINE__ before concatenation
#define REGISTER_SCRIPT_IMPL2(Type, Name, Line) \
    namespace { \
        struct ScriptReg_##Line { \
            ScriptReg_##Line() { \
                ::visutwin::canvas::ScriptFactories::instance().registerFactory( \
                    Name, [] { return std::make_unique<Type>(); } \
                ); \
            } \
        }; \
        static ScriptReg_##Line reg_instance_##Line; \
    }

#define REGISTER_SCRIPT_IMPL(Type, Name, Line) \
    REGISTER_SCRIPT_IMPL2(Type, Name, Line)

#define REGISTER_SCRIPT(Type, Name) \
    REGISTER_SCRIPT_IMPL(Type, Name, __LINE__)

namespace visutwin::canvas
{
    class Engine;

    using ScriptFactory = std::function<std::unique_ptr<Script>()>;

    // Global factory storage (separate from per-Engine registry)
    class ScriptFactories {
    public:
        static ScriptFactories& instance() {
            static ScriptFactories factories;
            return factories;
        }

        void registerFactory(const std::string& name, ScriptFactory factory) {
            _factories[name] = std::move(factory);
        }

        ScriptFactory getFactory(const std::string& name) const {
            auto it = _factories.find(name);
            return (it != _factories.end()) ? it->second : nullptr;
        }

    private:
        std::unordered_map<std::string, ScriptFactory> _factories;
    };

    /**
     * Container for all {@link ScriptType}s that are available to this application.
     */
    class ScriptRegistry
    {
    public:
        explicit ScriptRegistry(std::shared_ptr<Engine> engine): _engine(std::move(engine)) {}

        void registerType(const std::string& name, ScriptFactory factory);

        std::unique_ptr<Script> create(const std::string& name) const;

        template<typename T>
        void registerType() {
            registerType(T::scriptName(), [] { return std::make_unique<T>(); });
        }

    private:
        std::shared_ptr<Engine> _engine;

        std::unordered_map<std::string, ScriptFactory> _types;
    };
}
