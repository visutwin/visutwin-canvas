// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 28.12.2025
//
#pragma once

#include <core/eventHandler.h>

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
        virtual void fixedUpdate(float fixedDt) {}

        /*
         * Called for enabled (running state) scripts on each tick
         */
        virtual void update(float dt) {}

        /*
         * Called after all scripts update on each tick.
         */
        virtual void postUpdate(float dt) {}

        bool enabled() const;

    protected:
        Entity* entity() const { return _entity; }

    private:
        friend class ScriptComponent;

        bool _enabled = true;
        bool _initialized = false;
        bool _postInitialized = false;
        Entity* _entity = nullptr;
    };
}
