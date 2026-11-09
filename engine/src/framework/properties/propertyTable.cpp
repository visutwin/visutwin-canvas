// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
#include "propertyTable.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    const char* valueTypeName(const ValueType type)
    {
        switch (type) {
        case ValueType::Bool: return "bool";
        case ValueType::Int: return "int";
        case ValueType::Float: return "float";
        case ValueType::Vec2: return "vec2";
        case ValueType::Vec3: return "vec3";
        case ValueType::Vec4: return "vec4";
        case ValueType::Quat: return "quat";
        case ValueType::Color: return "color";
        case ValueType::String: return "string";
        case ValueType::Enum: return "enum";
        case ValueType::EntityRef: return "entity";
        }
        return "?";
    }

    const char* propertyStatusName(const PropertyStatus status)
    {
        switch (status) {
        case PropertyStatus::Ok: return "ok";
        case PropertyStatus::NoType: return "no such type";
        case PropertyStatus::NoProperty: return "no such property";
        case PropertyStatus::NoTarget: return "no target";
        case PropertyStatus::ReadOnly: return "read-only";
        case PropertyStatus::TypeMismatch: return "type mismatch";
        case PropertyStatus::InvalidValue: return "invalid value";
        }
        return "?";
    }

    bool valueHolds(const ValueType type, const PropertyValue& value)
    {
        switch (type) {
        case ValueType::Bool: return std::holds_alternative<bool>(value);
        case ValueType::Int:
        case ValueType::Enum: return std::holds_alternative<int>(value);
        case ValueType::Float: return std::holds_alternative<float>(value);
        case ValueType::Vec2: return std::holds_alternative<Vector2>(value);
        case ValueType::Vec3: return std::holds_alternative<Vector3>(value);
        case ValueType::Vec4: return std::holds_alternative<Vector4>(value);
        case ValueType::Quat: return std::holds_alternative<Quaternion>(value);
        case ValueType::Color: return std::holds_alternative<Color>(value);
        case ValueType::String: return std::holds_alternative<std::string>(value);
        case ValueType::EntityRef: return std::holds_alternative<EntityId>(value);
        }
        return false;
    }

    const EnumOption* PropertyDesc::option(const std::string_view optionName) const
    {
        for (const EnumOption& entry : options) {
            if (entry.name == optionName) {
                return &entry;
            }
        }
        return nullptr;
    }

    const EnumOption* PropertyDesc::option(const int value) const
    {
        for (const EnumOption& entry : options) {
            if (entry.value == value) {
                return &entry;
            }
        }
        return nullptr;
    }

    const PropertyDesc* TypeDesc::property(const std::string_view propertyName) const
    {
        for (const PropertyDesc& entry : properties) {
            if (entry.name == propertyName) {
                return &entry;
            }
        }
        return nullptr;
    }

    PropertyRegistry& PropertyRegistry::instance()
    {
        // Leaked, as the entity index is: a property may be read from a global's destructor.
        static PropertyRegistry* registry = [] {
            auto* created = new PropertyRegistry();
            registerBuiltinProperties(*created);
            return created;
        }();
        return *registry;
    }

    TypeDesc& PropertyRegistry::addType(std::string name, std::string description)
    {
        assert(!type(name) && "a property type of this name is already registered");
        auto type = std::make_unique<TypeDesc>();
        type->name = std::move(name);
        type->description = std::move(description);
        _types.push_back(std::move(type));
        return *_types.back();
    }

    const TypeDesc* PropertyRegistry::type(const std::string_view typeName) const
    {
        for (const auto& entry : _types) {
            if (entry->name == typeName) {
                return entry.get();
            }
        }
        return nullptr;
    }

    std::pair<const TypeDesc*, const PropertyDesc*> PropertyRegistry::find(const std::string_view qualifiedName) const
    {
        const size_t dot = qualifiedName.find('.');
        if (dot == std::string_view::npos) {
            return {nullptr, nullptr};
        }
        const TypeDesc* found = type(qualifiedName.substr(0, dot));
        return {found, found ? found->property(qualifiedName.substr(dot + 1)) : nullptr};
    }

    PropertyValue PropertyRegistry::get(const PropertyDesc& property, const void* target)
    {
        return property.get(target);
    }

    PropertyStatus PropertyRegistry::set(const PropertyDesc& property, void* target, const PropertyValue& value)
    {
        if (!target) {
            return PropertyStatus::NoTarget;
        }
        if (property.readOnly()) {
            return PropertyStatus::ReadOnly;
        }
        if (!valueHolds(property.type, value)) {
            return PropertyStatus::TypeMismatch;
        }
        if (property.type == ValueType::Enum && !property.option(std::get<int>(value))) {
            return PropertyStatus::InvalidValue;
        }
        property.set(target, value);
        return PropertyStatus::Ok;
    }

    namespace
    {
        PropertyStatus resolveTarget(const PropertyRegistry& registry, Entity& entity,
            const std::string_view qualifiedName, const PropertyDesc*& property, void*& target)
        {
            const auto [type, found] = registry.find(qualifiedName);
            if (!type) {
                return PropertyStatus::NoType;
            }
            if (!found) {
                return PropertyStatus::NoProperty;
            }
            target = type->resolve ? type->resolve(entity) : nullptr;
            if (!target) {
                return PropertyStatus::NoTarget;
            }
            property = found;
            return PropertyStatus::Ok;
        }
    }

    PropertyStatus PropertyRegistry::get(Entity& entity, const std::string_view qualifiedName, PropertyValue& out) const
    {
        const PropertyDesc* property = nullptr;
        void* target = nullptr;
        if (const PropertyStatus status = resolveTarget(*this, entity, qualifiedName, property, target);
            status != PropertyStatus::Ok) {
            return status;
        }
        out = property->get(target);
        return PropertyStatus::Ok;
    }

    PropertyStatus PropertyRegistry::set(Entity& entity, const std::string_view qualifiedName,
        const PropertyValue& value) const
    {
        const PropertyDesc* property = nullptr;
        void* target = nullptr;
        if (const PropertyStatus status = resolveTarget(*this, entity, qualifiedName, property, target);
            status != PropertyStatus::Ok) {
            return status;
        }
        return set(*property, target, value);
    }

    namespace
    {
        // The shortest of %.6g .. %.9g that reads back as the same float (%.9g always does).
        std::string formatFloat(const float value)
        {
            char buffer[32];
            for (int precision = 6; precision <= 9; ++precision) {
                std::snprintf(buffer, sizeof(buffer), "%.*g", precision, static_cast<double>(value));
                if (std::strtof(buffer, nullptr) == value) {
                    break;
                }
            }
            return buffer;
        }

        std::string formatFloats(std::initializer_list<float> values)
        {
            std::string result;
            for (const float value : values) {
                if (!result.empty()) {
                    result += ", ";
                }
                result += formatFloat(value);
            }
            return result;
        }

        std::string_view trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                text.remove_prefix(1);
            }
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
                text.remove_suffix(1);
            }
            return text;
        }

        bool parseFloat(const std::string_view text, float& out)
        {
            const std::string copy(trim(text));
            if (copy.empty()) {
                return false;
            }
            char* end = nullptr;
            out = std::strtof(copy.c_str(), &end);
            return end == copy.c_str() + copy.size();
        }

        bool parseInt(const std::string_view text, int& out)
        {
            const std::string copy(trim(text));
            if (copy.empty()) {
                return false;
            }
            char* end = nullptr;
            const long value = std::strtol(copy.c_str(), &end, 10);
            out = static_cast<int>(value);
            return end == copy.c_str() + copy.size() && value == out;
        }

        // Exactly `count` comma-separated floats.
        bool parseFloats(std::string_view text, float* out, const size_t count)
        {
            for (size_t i = 0; i < count; ++i) {
                const size_t comma = text.find(',');
                const bool last = i + 1 == count;
                if (last != (comma == std::string_view::npos)) {
                    return false;
                }
                if (!parseFloat(text.substr(0, comma), out[i])) {
                    return false;
                }
                text = last ? std::string_view() : text.substr(comma + 1);
            }
            return true;
        }
    }

    std::string formatPropertyValue(const PropertyDesc& property, const PropertyValue& value)
    {
        if (!valueHolds(property.type, value)) {
            return "<" + std::string(valueTypeName(property.type)) + "?>";
        }
        switch (property.type) {
        case ValueType::Bool: return std::get<bool>(value) ? "true" : "false";
        case ValueType::Int: return std::to_string(std::get<int>(value));
        case ValueType::Enum: {
            const int number = std::get<int>(value);
            const EnumOption* option = property.option(number);
            return option ? option->name : std::to_string(number);
        }
        case ValueType::Float: return formatFloat(std::get<float>(value));
        case ValueType::Vec2: {
            const auto& v = std::get<Vector2>(value);
            return formatFloats({v.x, v.y});
        }
        case ValueType::Vec3: {
            const auto& v = std::get<Vector3>(value);
            return formatFloats({v.getX(), v.getY(), v.getZ()});
        }
        case ValueType::Vec4: {
            const auto& v = std::get<Vector4>(value);
            return formatFloats({v.getX(), v.getY(), v.getZ(), v.getW()});
        }
        case ValueType::Quat: {
            const auto& q = std::get<Quaternion>(value);
            return formatFloats({q.getX(), q.getY(), q.getZ(), q.getW()});
        }
        case ValueType::Color: {
            const auto& c = std::get<Color>(value);
            return formatFloats({c.r, c.g, c.b, c.a});
        }
        case ValueType::String: return std::get<std::string>(value);
        case ValueType::EntityRef: return std::to_string(std::get<EntityId>(value).value);
        }
        return {};
    }

    bool parsePropertyValue(const PropertyDesc& property, const std::string_view text, PropertyValue& out)
    {
        float f[4];
        switch (property.type) {
        case ValueType::Bool: {
            const std::string_view word = trim(text);
            if (word == "true" || word == "1") {
                out = true;
                return true;
            }
            if (word == "false" || word == "0") {
                out = false;
                return true;
            }
            return false;
        }
        case ValueType::Int: {
            int value = 0;
            if (!parseInt(text, value)) {
                return false;
            }
            out = value;
            return true;
        }
        case ValueType::Enum: {
            if (const EnumOption* option = property.option(trim(text))) {
                out = option->value;
                return true;
            }
            int value = 0;
            if (!parseInt(text, value)) {
                return false;
            }
            out = value;
            return true;
        }
        case ValueType::Float:
            if (!parseFloat(text, f[0])) {
                return false;
            }
            out = f[0];
            return true;
        case ValueType::Vec2:
            if (!parseFloats(text, f, 2)) {
                return false;
            }
            out = Vector2(f[0], f[1]);
            return true;
        case ValueType::Vec3:
            if (!parseFloats(text, f, 3)) {
                return false;
            }
            out = Vector3(f[0], f[1], f[2]);
            return true;
        case ValueType::Vec4:
            if (!parseFloats(text, f, 4)) {
                return false;
            }
            out = Vector4(f[0], f[1], f[2], f[3]);
            return true;
        case ValueType::Quat:
            if (!parseFloats(text, f, 4)) {
                return false;
            }
            out = Quaternion(f[0], f[1], f[2], f[3]);
            return true;
        case ValueType::Color:
            // rgb, or rgba.
            if (parseFloats(text, f, 4)) {
                out = Color(f[0], f[1], f[2], f[3]);
                return true;
            }
            if (parseFloats(text, f, 3)) {
                out = Color(f[0], f[1], f[2], 1.0f);
                return true;
            }
            return false;
        case ValueType::String:
            out = std::string(text);
            return true;
        case ValueType::EntityRef: {
            const std::string copy(trim(text));
            char* end = nullptr;
            const unsigned long long value = std::strtoull(copy.c_str(), &end, 10);
            if (copy.empty() || end != copy.c_str() + copy.size()) {
                return false;
            }
            out = EntityId{value};
            return true;
        }
        }
        return false;
    }

    namespace
    {
        void describeObject(const TypeDesc& type, const void* target, std::string& out)
        {
            for (const PropertyDesc& property : type.properties) {
                out += type.name;
                out += '.';
                out += property.name;
                out += " = ";
                out += formatPropertyValue(property, property.get(target));
                if (property.readOnly()) {
                    out += "  (read-only)";
                }
                out += '\n';
            }
        }
    }

    std::string describeEntityProperties(Entity& entity)
    {
        const PropertyRegistry& registry = PropertyRegistry::instance();
        std::string out;
        for (const auto& type : registry.types()) {
            if (!type->resolve) {
                continue;
            }
            if (const void* target = type->resolve(entity)) {
                describeObject(*type, target, out);
            }
        }
        // The material a render component draws with, when it is a standard one.
        if (auto* render = entity.findComponent<RenderComponent>()) {
            const TypeDesc* standard = registry.type("standard");
            Material* material = render->material();
            if (!material && !render->meshInstances().empty() && render->meshInstances().front()) {
                material = render->meshInstances().front()->material();
            }
            if (auto* standardMaterial = dynamic_cast<StandardMaterial*>(material); standard && standardMaterial) {
                describeObject(*standard, standardMaterial, out);
            }
        }
        return out;
    }

    PropertyStatus applyPropertyAssignment(Engine& engine, const std::string_view assignment)
    {
        const size_t colon = assignment.find(':');
        const size_t equals = assignment.find('=', colon == std::string_view::npos ? 0 : colon);
        if (colon == std::string_view::npos || equals == std::string_view::npos) {
            return PropertyStatus::TypeMismatch;
        }
        Entity* entity = engine.entityByPath(trim(assignment.substr(0, colon)));
        if (!entity) {
            return PropertyStatus::NoTarget;
        }
        const std::string_view name = trim(assignment.substr(colon + 1, equals - colon - 1));
        const PropertyRegistry& registry = PropertyRegistry::instance();
        const auto [type, property] = registry.find(name);
        if (!type) {
            return PropertyStatus::NoType;
        }
        if (!property) {
            return PropertyStatus::NoProperty;
        }
        PropertyValue value;
        if (!parsePropertyValue(*property, assignment.substr(equals + 1), value)) {
            return PropertyStatus::TypeMismatch;
        }
        return registry.set(*entity, name, value);
    }
}
