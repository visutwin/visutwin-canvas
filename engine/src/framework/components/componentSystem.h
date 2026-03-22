// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//

#pragma once

#include <string>
#include <typeinfo>

#include "component.h"
#include "framework/engine.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    class IComponentSystem
    {
    public:
        virtual ~IComponentSystem() = default;
        IComponentSystem(Engine* engine, const std::string& id) : _engine(engine), _id(id) {}

        virtual std::unique_ptr<Component> addComponent(Entity* entity) = 0;

        [[nodiscard]] const std::string& id() const { return _id; }
        Engine* engine() const { return _engine; }

        virtual const std::type_info& componentType() const = 0;
    protected:
        std::string _id;
        Engine* _engine;
    };

    /*
    * Component Systems contain the logic and functionality to update all Components of a particular type
    */
    template <class ComponentType, class DataType>
    class ComponentSystem : public IComponentSystem
    {
    public:
        ComponentSystem(Engine* engine, const std::string& id) : IComponentSystem(engine, id) {}

        // Create new Component and component data instances and attach them to the entity.
        std::unique_ptr<Component> addComponent(Entity* entity) override;

        const std::type_info& componentType() const override { return typeid(ComponentType); }
    };
}

#include "componentSystem.inl"
