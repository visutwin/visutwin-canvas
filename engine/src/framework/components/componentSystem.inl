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

        std::unique_ptr<DataType> data = std::make_unique<DataType>();

        component->initializeComponentData();

        return component;
    }
}