// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/sortedLoopArray.h"
#include "scriptComponent.h"
#include "framework/components/componentSystem.h"

namespace visutwin::canvas
{
    /*
     * Allows scripts to be attached to an Entity and executed
     */
    class ScriptComponentSystem : public ComponentSystem<ScriptComponent, ScriptComponentData>
    {
    public:
        ScriptComponentSystem(Engine* engine)
            : ComponentSystem(engine, "script"),
              _components(executionOrderKey),
              _phaseComponents{SortedLoopArray<ScriptComponent*>(executionOrderKey),
                               SortedLoopArray<ScriptComponent*>(executionOrderKey),
                               SortedLoopArray<ScriptComponent*>(executionOrderKey)}
        {
        }

        std::unique_ptr<Component> addComponent(Entity* entity) override
        {
            auto component = std::make_unique<ScriptComponent>(this, entity);
            component->initializeComponentData();
            // Rising creation order, set directly (the setter re-sorts), and INSERTED by
            // it: usually at the end, but after setExecutionOrder has moved another
            // component past the counter an append would leave the list unsorted.
            component->_executionOrder = _executionCounter++;
            _components.insert(component.get());
            return component;
        }

        // Called from ~ScriptComponent so a destroyed component (e.g. its entity was
        // deleted) is never left dangling in the update list. NOT the virtual
        // removeComponent(Entity*) — this is bookkeeping for a component that is
        // already being destroyed, and an overload here would hide the virtual.
        void unregisterComponent(ScriptComponent* component)
        {
            _components.remove(component);
            for (int phase = 0; phase < kPhaseCount; ++phase) {
                if ((component->_phases & (1u << phase)) != 0) {
                    _phaseComponents[phase].remove(component);
                }
            }
        }

        /// A component gained its first script implementing each phase in `gained`
        /// (Script::Phase bits): it joins those phases' lists, in execution order. The
        /// phase loops walk only these lists, so a component none of whose scripts
        /// implements a phase is not even read in it. Joining mid-loop is safe, as
        /// insert() keeps the loop index on the component being run.
        void componentGainedPhases(ScriptComponent* component, const uint8_t gained)
        {
            for (int phase = 0; phase < kPhaseCount; ++phase) {
                if ((gained & (1u << phase)) != 0) {
                    _phaseComponents[phase].insert(component);
                }
            }
        }

        /// Re-sort the lists after a component's execution order changed. Each keeps
        /// its running loop index pointing at the same component.
        void sortComponents()
        {
            _components.sort();
            for (auto& list : _phaseComponents) {
                list.sort();
            }
        }

        /// Application-wide initialize phase, fired once from Engine::start(). Every
        /// script initializes before any script post-initializes, which is the
        /// contract Script::postInitialize documents and which nothing honoured
        /// before: the phase existed but nothing ever fired it.
        void initialize()
        {
            // Same loopIndex walk the update phases use, so a script that adds or
            // removes a component while initializing does not invalidate the sweep.
            for (_components.loopIndex = 0;
                 _components.loopIndex < static_cast<int>(_components.length);
                 _components.loopIndex++) {
                auto* component = _components.items[_components.loopIndex];
                if (component && component->active()) {
                    component->initializeScripts();
                }
            }
        }

        void postInitialize()
        {
            for (_components.loopIndex = 0;
                 _components.loopIndex < static_cast<int>(_components.length);
                 _components.loopIndex++) {
                auto* component = _components.items[_components.loopIndex];
                if (component && component->active()) {
                    component->postInitializeScripts();
                }
            }
        }

        void fixedUpdate(float fixedDt)
        {
            auto& list = _phaseComponents[kFixedUpdateList];
            for (list.loopIndex = 0; list.loopIndex < static_cast<int>(list.length); list.loopIndex++) {
                if (auto* component = list.items[list.loopIndex]) {
                    component->fixedUpdateScripts(fixedDt);
                }
            }
        }

        void update(float dt)
        {
            auto& list = _phaseComponents[kUpdateList];
            for (list.loopIndex = 0; list.loopIndex < static_cast<int>(list.length); list.loopIndex++) {
                if (auto* component = list.items[list.loopIndex]) {
                    component->updateScripts(dt);
                }
            }
        }

        void postUpdate(float dt)
        {
            auto& list = _phaseComponents[kPostUpdateList];
            for (list.loopIndex = 0; list.loopIndex < static_cast<int>(list.length); list.loopIndex++) {
                if (auto* component = list.items[list.loopIndex]) {
                    component->postUpdateScripts(dt);
                }
            }
        }

        /// How many components the phase's loop visits (Script::Phase bit).
        [[nodiscard]] size_t phaseComponentCount(const uint8_t phase) const
        {
            for (int index = 0; index < kPhaseCount; ++index) {
                if (phase == (1u << index)) {
                    return _phaseComponents[index].length;
                }
            }
            return 0;
        }

    private:
        static float executionOrderKey(ScriptComponent* const& component)
        {
            return component ? static_cast<float>(component->executionOrder()) : 0.0f;
        }

        // One list per Script::Phase bit, indexed by the bit's position.
        static constexpr int kPhaseCount = 3;
        static constexpr int kUpdateList = 0;
        static constexpr int kPostUpdateList = 1;
        static constexpr int kFixedUpdateList = 2;
        static_assert(Script::PHASE_UPDATE == 1u << kUpdateList && Script::PHASE_POST_UPDATE == 1u << kPostUpdateList &&
            Script::PHASE_FIXED_UPDATE == 1u << kFixedUpdateList);

        // Every component, for the initialize phases.
        SortedLoopArray<ScriptComponent*> _components;
        // The components with a script implementing each phase.
        SortedLoopArray<ScriptComponent*> _phaseComponents[kPhaseCount];
        int _executionCounter = 0;
    };
}
