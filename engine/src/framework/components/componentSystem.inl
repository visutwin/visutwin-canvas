//
// Created by Arnis Lektauers on 10.09.2025.
//
#pragma once

#include <memory>

namespace visutwin::canvas
{
    template <class ComponentType, class DataType>
    std::unique_ptr<Component> ComponentSystem<ComponentType, DataType>::addComponent(Entity* entity)
    {
        std::unique_ptr<ComponentType> component = std::make_unique<ComponentType>(this, entity);

        // NOTE: DataType is not instantiated. It used to be heap-allocated here and
        // dropped on the next line — initializeComponentData() takes no arguments, so
        // nothing could ever reach it.

        component->initializeComponentData();

        fire("add", entity, component.get());

        return component;
    }

    template <class ComponentType, class DataType>
    bool ComponentSystem<ComponentType, DataType>::removeComponent(Entity* entity)
    {
        if (!entity) {
            return false;
        }
        Component* component = entity->template findComponent<ComponentType>();
        if (!component) {
            return false;
        }

        // While it is still valid, so a listener can read it.
        fire("beforeremove", entity, component);
        const bool removed =
            entity->removeComponentInstance(componentTypeID<ComponentType>());
        if (removed) {
            fire("remove", entity);
        }
        return removed;
    }
}