// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
// The live components of one engine, a creation-ordered list per component type.
//
#pragma once

#include <memory>
#include <vector>

#include "component.h"
#include "componentInstanceList.h"

namespace visutwin::canvas
{
    class MeshInstance;

    /**
     * @brief The components that belong to one engine, by type.
     * @ingroup group_framework_ecs
     *
     * Every Engine owns one (`Engine::components()`), and everything that sweeps the
     * components of a type — the renderer, the systems, the UI, the examples — sweeps
     * this engine's list, so two engines in one process never see each other's
     * components.
     *
     * A component joins the registry of the engine it belongs to as it is constructed:
     * its system's engine, or, for a component built with no system (the glTF container
     * builds its entities that way), the engine of the hierarchy its entity is in. A
     * component whose entity is in no engine's hierarchy yet joins when the entity is
     * inserted under that engine's root (Entity::onInsertedIntoParent), still at its
     * CREATION position: every list is ordered by a process-wide creation serial, which
     * is the order equal sort keys keep, lights fill their slots and scripts run in.
     *
     * A component that outlives its engine (its entity detached and held elsewhere) is
     * let go by the registry's destructor and belongs to no registry from then on.
     *
     * Main thread only, like the components themselves.
     */
    class ComponentRegistry
    {
    public:
        ComponentRegistry() = default;
        ComponentRegistry(const ComponentRegistry&) = delete;
        ComponentRegistry& operator=(const ComponentRegistry&) = delete;

        ~ComponentRegistry()
        {
            for (auto& list : _lists) {
                if (list) {
                    list->release();
                }
            }
        }

        /// This engine's live components of type T, in creation order, with no holes.
        template <class T>
        const std::vector<T*>& instances()
        {
            return list<T>().items();
        }

        /// The list itself (forEachLive, pendingHoles).
        template <class T>
        ComponentInstanceList<T>& list()
        {
            const ComponentTypeID id = componentTypeID<T>();
            if (id >= _lists.size()) {
                _lists.resize(id + 1);
            }
            if (!_lists[id]) {
                _lists[id] = std::make_unique<TypedList<T>>();
            }
            return static_cast<TypedList<T>*>(_lists[id].get())->items;
        }

        /// The live batch mesh instances of this engine's BatchManager. They belong to no
        /// RenderComponent (the batcher registers them straight with the scene layers), so
        /// the shadow caster collector reads them from here. Maintained by BatchManager.
        std::vector<MeshInstance*>& batchMeshInstances() { return _batchMeshInstances; }

    private:
        struct ListBase
        {
            virtual ~ListBase() = default;
            /// Lets go of every component still listed (the registry is going away).
            virtual void release() = 0;
        };

        template <class T>
        struct TypedList final : ListBase
        {
            ComponentInstanceList<T> items;

            void release() override
            {
                items.forEachLive([](T* component) {
                    static_cast<Component*>(component)->_registry = nullptr;
                });
            }
        };

        // Indexed by componentTypeID: the ids are small consecutive integers.
        std::vector<std::unique_ptr<ListBase>> _lists;
        std::vector<MeshInstance*> _batchMeshInstances;
    };

    template <class T>
    void Component::listInstance(T* /*self*/)
    {
        _instanceSerial = ++_lastInstanceSerial;
        _listAdd = [](ComponentRegistry& registry, Component* component) {
            registry.list<T>().add(static_cast<T*>(component));
        };
        _listRemove = [](ComponentRegistry& registry, Component* component) {
            registry.list<T>().remove(static_cast<T*>(component));
        };
        joinRegistry(resolveRegistry());
    }

    /// The components of type T in `registry`, or none when there is no registry (a
    /// renderer with no scene, a component in no engine's hierarchy).
    template <class T>
    const std::vector<T*>& instancesOf(ComponentRegistry* registry)
    {
        if (registry) {
            return registry->instances<T>();
        }
        static const std::vector<T*> none;
        return none;
    }
}
