// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// A table of the engine's editable state: each TYPE (the entity node, a component, a material)
// lists its PROPERTIES by stable name, each with its value type, a getter, a setter when it is
// writable, and editor hints (range, step, unit, enum names). Anything that edits or shows the
// engine generically — the C API a frontend in another language binds to, a text dump, an
// inspector — works through this table instead of knowing each class, so a property added
// here reaches all of them.
//
// A property is named "<type>.<property>" ("light.intensity"); the target of a type is found
// from an entity by the type's resolver (the entity itself, one of its components), or handed
// over directly for a type no entity carries (a material). Values are a PropertyValue of the
// property's type; an enum travels as its integer value and is named by its options.
//
#pragma once

#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "framework/entityId.h"

namespace visutwin::canvas
{
    class Engine;
    class Entity;

    enum class ValueType
    {
        Bool,
        Int,
        Float,
        Vec2,
        Vec3,
        Vec4,
        Quat,
        Color,
        String,
        /// An int whose meaning is one of the property's options.
        Enum,
        /// Another entity, by id (0 for none).
        EntityRef
    };

    const char* valueTypeName(ValueType type);

    /// A property value; Int and Enum both hold an int.
    using PropertyValue = std::variant<std::monostate, bool, int, float, Vector2, Vector3, Vector4, Quaternion,
        Color, std::string, EntityId>;

    /// Whether `value` holds what a property of `type` takes.
    bool valueHolds(ValueType type, const PropertyValue& value);

    struct EnumOption
    {
        int value;
        std::string name;
    };

    struct PropertyDesc
    {
        std::string name;
        ValueType type = ValueType::Float;
        std::function<PropertyValue(const void* target)> get;
        /// Empty for a read-only property.
        std::function<void(void* target, const PropertyValue& value)> set;

        // Editor hints; nothing enforces them.
        float min = -std::numeric_limits<float>::infinity();
        float max = std::numeric_limits<float>::infinity();
        float step = 0.0f;
        std::string unit;
        std::string description;
        /// The values an Enum property takes, with their names.
        std::vector<EnumOption> options;

        bool readOnly() const { return !set; }
        /// The option named `name`, or null.
        const EnumOption* option(std::string_view name) const;
        /// The option with value `value`, or null.
        const EnumOption* option(int value) const;
    };

    struct TypeDesc
    {
        /// The stable name ("node", "light", "standard").
        std::string name;
        std::string description;
        std::vector<PropertyDesc> properties;
        /// The type's object on an entity, or null when the entity has none. Empty for a type
        /// no entity carries, whose objects are handed over directly.
        std::function<void*(Entity& entity)> resolve;

        const PropertyDesc* property(std::string_view propertyName) const;
    };

    enum class PropertyStatus
    {
        Ok,
        NoType,
        NoProperty,
        /// The entity has no object of the type (no such component).
        NoTarget,
        ReadOnly,
        /// The value is not of the property's type.
        TypeMismatch,
        /// An enum value that is none of the options.
        InvalidValue
    };

    const char* propertyStatusName(PropertyStatus status);

    namespace property_detail
    {
        template <class R>
        constexpr ValueType valueTypeOf()
        {
            if constexpr (std::is_same_v<R, bool>) {
                return ValueType::Bool;
            } else if constexpr (std::is_enum_v<R>) {
                return ValueType::Enum;
            } else if constexpr (std::is_same_v<R, int>) {
                return ValueType::Int;
            } else if constexpr (std::is_same_v<R, float>) {
                return ValueType::Float;
            } else if constexpr (std::is_same_v<R, Vector2>) {
                return ValueType::Vec2;
            } else if constexpr (std::is_same_v<R, Vector3>) {
                return ValueType::Vec3;
            } else if constexpr (std::is_same_v<R, Vector4>) {
                return ValueType::Vec4;
            } else if constexpr (std::is_same_v<R, Quaternion>) {
                return ValueType::Quat;
            } else if constexpr (std::is_same_v<R, Color>) {
                return ValueType::Color;
            } else if constexpr (std::is_same_v<R, std::string>) {
                return ValueType::String;
            } else if constexpr (std::is_same_v<R, EntityId>) {
                return ValueType::EntityRef;
            } else {
                static_assert(sizeof(R) == 0, "a property of this C++ type has no ValueType");
                return ValueType::Bool;
            }
        }

        template <class R>
        PropertyValue toValue(const R& value)
        {
            if constexpr (std::is_enum_v<R>) {
                return static_cast<int>(value);
            } else {
                return value;
            }
        }

        template <class R>
        R fromValue(const PropertyValue& value)
        {
            if constexpr (std::is_enum_v<R>) {
                return static_cast<R>(std::get<int>(value));
            } else {
                return std::get<R>(value);
            }
        }
    }

    /// Registers the properties of a type whose objects are T. A getter is a member function or
    /// a callable taking `const T&`; a setter a member function or a callable taking `T&` and the
    /// value. The value type follows from what the getter returns. The hint setters apply to the
    /// property added last.
    template <class T>
    class TypeBuilder
    {
    public:
        explicit TypeBuilder(TypeDesc& type) : _type(type) {}

        template <class Getter>
        TypeBuilder& property(std::string name, Getter getter)
        {
            using R = std::decay_t<std::invoke_result_t<Getter, const T&>>;
            PropertyDesc desc;
            desc.name = std::move(name);
            desc.type = property_detail::valueTypeOf<R>();
            desc.get = [getter](const void* target) {
                return property_detail::toValue<R>(std::invoke(getter, *static_cast<const T*>(target)));
            };
            _type.properties.push_back(std::move(desc));
            return *this;
        }

        template <class Getter, class Setter>
        TypeBuilder& property(std::string name, Getter getter, Setter setter)
        {
            property(std::move(name), getter);
            using R = std::decay_t<std::invoke_result_t<Getter, const T&>>;
            _type.properties.back().set = [setter](void* target, const PropertyValue& value) {
                std::invoke(setter, *static_cast<T*>(target), property_detail::fromValue<R>(value));
            };
            return *this;
        }

        TypeBuilder& range(const float min, const float max, const float step = 0.0f)
        {
            last().min = min;
            last().max = max;
            last().step = step;
            return *this;
        }

        TypeBuilder& unit(std::string value)
        {
            last().unit = std::move(value);
            return *this;
        }

        TypeBuilder& describe(std::string value)
        {
            last().description = std::move(value);
            return *this;
        }

        TypeBuilder& options(std::vector<EnumOption> values)
        {
            last().options = std::move(values);
            return *this;
        }

    private:
        PropertyDesc& last() { return _type.properties.back(); }

        TypeDesc& _type;
    };

    /// The process-wide table. The engine's own types are registered on first use; an
    /// application may add its own (a script's settings, say) with registerType.
    class PropertyRegistry
    {
    public:
        static PropertyRegistry& instance();

        /// A new type; its name must not be taken. `resolve` finds its T on an entity, or is
        /// empty for a type no entity carries.
        template <class T>
        TypeBuilder<T> registerType(std::string name, std::string description,
            std::function<T*(Entity&)> resolve = {})
        {
            TypeDesc& type = addType(std::move(name), std::move(description));
            if (resolve) {
                type.resolve = [resolve = std::move(resolve)](Entity& entity) -> void* { return resolve(entity); };
            }
            return TypeBuilder<T>(type);
        }

        /// Every type, in registration order (each pointer stable for the process).
        const std::vector<std::unique_ptr<TypeDesc>>& types() const { return _types; }
        const TypeDesc* type(std::string_view typeName) const;

        /// "light.intensity" split and found, or nulls.
        std::pair<const TypeDesc*, const PropertyDesc*> find(std::string_view qualifiedName) const;

        /// Read or write `type.property` on an entity, through the type's resolver.
        PropertyStatus get(Entity& entity, std::string_view qualifiedName, PropertyValue& out) const;
        PropertyStatus set(Entity& entity, std::string_view qualifiedName, const PropertyValue& value) const;

        /// Read or write one property of an object of its type (a material, or a resolved target).
        static PropertyValue get(const PropertyDesc& property, const void* target);
        static PropertyStatus set(const PropertyDesc& property, void* target, const PropertyValue& value);

    private:
        PropertyRegistry() = default;
        TypeDesc& addType(std::string name, std::string description);

        std::vector<std::unique_ptr<TypeDesc>> _types;
    };

    /// Registers the engine's own types (builtinProperties.cpp); instance() calls it once.
    void registerBuiltinProperties(PropertyRegistry& registry);

    /// A value as text: numbers in shortest round-trip form, vectors comma-separated, an enum
    /// by its option name (its number when it names none), an entity by id.
    std::string formatPropertyValue(const PropertyDesc& property, const PropertyValue& value);

    /// The text formatPropertyValue writes, read back as a value of the property's type (a bool
    /// also from 1/0, an enum also from its number). False when the text is not one.
    bool parsePropertyValue(const PropertyDesc& property, std::string_view text, PropertyValue& out);

    /// Every property of every type the entity carries, one "type.property = value" line each,
    /// and for a render component's standard material its "standard." lines.
    std::string describeEntityProperties(Entity& entity);

    /// "path:type.property=value" (path as Entity::path; the text as parsePropertyValue reads
    /// it) applied to an entity of the engine's hierarchy. Returns the status, NoTarget also for
    /// a path naming no entity, TypeMismatch for unreadable text or a malformed assignment.
    PropertyStatus applyPropertyAssignment(Engine& engine, std::string_view assignment);
}
