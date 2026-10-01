// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 28.12.2025
//
#pragma once

#include <concepts>
#include <cstdint>
#include <memory>

#include <core/eventHandler.h>

#include "framework/components/component.h"

#define SCRIPT_NAME(Name) \
    static constexpr const char* scriptName() { return Name; }

namespace visutwin::canvas
{
    class Entity;
    class ScriptComponent;

    /**
     * The Script class is the fundamental base class for all scripts within VisuTwin Canvas. It provides
     * the minimal interface required for a script to be compatible with both the Engine and the
     * Editor.
     *
     * At its core, a script is simply a collection of methods that are called at various points in the
     * Engine's lifecycle. These methods are:
     *
     * - initialize() - Called once when the script is initialized.
     * - postInitialize() - Called once after all scripts have been initialized.
     * - update(dt) - Called every frame, if the script is enabled.
     * - postUpdate(dt) - Called every frame, after all scripts have been updated.
     * - swap(old) - Called when a script is redefined (hot-swapping).
     *
     * These methods are entirely optional but provide a useful way to manage the lifecycle of a
     * script and perform any necessary setup and cleanup.
     *
     * @category Script
     */
    class Script: public EventHandler
    {
    public:
        /**
         * Called when script is about to run for the first time.
         * Override this method in subclasses to implement custom initialization logic.
         */
        virtual void initialize() {}

        /**
         * Called after all initialize methods are executed in the same tick or enabling chain of actions.
         * Override this method in subclasses to implement post-initialization logic.
         */
        virtual void postInitialize() {}

        /*
         * Called at a fixed interval for deterministic simulation (physics, etc.).
         * The fixedDt is constant across calls (default 1/60s).
         */
        virtual void fixedUpdate(float /*fixedDt*/) {}

        /*
         * Called for enabled (running state) scripts on each tick
         */
        virtual void update(float /*dt*/) {}

        /*
         * Called after all scripts update on each tick.
         */
        virtual void postUpdate(float /*dt*/) {}

        /*
         * Called on a script that Entity::clone created, before it initializes, with the
         * source entity's script of the same name. It stands in for upstream's copy of
         * script ATTRIBUTES, which this port does not have: copy whatever configuration
         * the clone should share. The default copies nothing, so a clone starts from the
         * script's defaults.
         */
        virtual void cloneFrom(const Script& /*source*/) {}

        /*
         * Called once the whole cloned subtree exists (upstream remaps entity-typed
         * attributes here): point a reference into the source subtree at its copy with
         * Component::remapCloned(pointer, map).
         */
        virtual void resolveClonedReferences(const Script& /*source*/, const CloneNodeMap& /*map*/) {}

        bool enabled() const;

        /// The per-frame phases a script can take part in, one bit each.
        enum Phase : uint8_t
        {
            PHASE_UPDATE = 1,
            PHASE_POST_UPDATE = 2,
            PHASE_FIXED_UPDATE = 4,
            PHASE_ALL = PHASE_UPDATE | PHASE_POST_UPDATE | PHASE_FIXED_UPDATE
        };

        /// The phases T OVERRIDES — itself or through a base between it and Script —
        /// decided at compile time: `&T::update` names Script's own member exactly when
        /// nothing overrode it. A script is only visited in a phase it implements, as
        /// upstream keeps a script out of its update list when it defines no `update`;
        /// calling every script in all three phases is three sweeps of every script in
        /// the application for scripts that mostly implement one. Where the member
        /// cannot be named (it is overloaded, or not accessible) the phase counts as
        /// implemented, which is always safe.
        template <typename T>
        static constexpr uint8_t phasesOf()
        {
            uint8_t phases = 0;
            if (!requires { requires std::same_as<decltype(&T::update), void (Script::*)(float)>; }) {
                phases |= PHASE_UPDATE;
            }
            if (!requires { requires std::same_as<decltype(&T::postUpdate), void (Script::*)(float)>; }) {
                phases |= PHASE_POST_UPDATE;
            }
            if (!requires { requires std::same_as<decltype(&T::fixedUpdate), void (Script::*)(float)>; }) {
                phases |= PHASE_FIXED_UPDATE;
            }
            return phases;
        }

        /// A new T that knows its phases. What the registries' factories call; a script
        /// made any other way keeps PHASE_ALL and is visited in every phase.
        template <typename T>
        static std::unique_ptr<Script> make()
        {
            std::unique_ptr<Script> script = std::make_unique<T>();
            script->_phases = phasesOf<T>();
            return script;
        }

        /// The phases this script is visited in (see phasesOf).
        uint8_t phases() const { return _phases; }

    protected:
        Entity* entity() const { return _entity; }

    private:
        friend class ScriptComponent;

        bool _enabled = true;
        bool _initialized = false;
        bool _postInitialized = false;
        uint8_t _phases = PHASE_ALL;
        Entity* _entity = nullptr;
    };
}
