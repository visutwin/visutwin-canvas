// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// C API: the schema (types and properties) and property values.
#include "capiInternal.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::capi;

extern "C" {

// ---- Schema ---------------------------------------------------------------------------

vt_status vt_type_count(uint32_t* out)
{
    return guarded([&]() -> vt_status {
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = static_cast<uint32_t>(PropertyRegistry::instance().types().size());
        return VT_OK;
    });
}

vt_status vt_type_at(const uint32_t index, vt_type* out)
{
    return guarded([&]() -> vt_status {
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        if (index >= PropertyRegistry::instance().types().size()) {
            return fail(VT_ERROR_NO_TYPE, "no type at index " + std::to_string(index));
        }
        *out = index + 1;
        return VT_OK;
    });
}

vt_status vt_type_find(const char* name, vt_type* out)
{
    return guarded([&]() -> vt_status {
        if (!name || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no name or no output");
        }
        const TypeDesc* type = PropertyRegistry::instance().type(name);
        if (!type) {
            return fail(VT_ERROR_NO_TYPE, std::string("no type ") + name);
        }
        *out = handleOf(type);
        return VT_OK;
    });
}

vt_status vt_type_get_info(const vt_type handle, vt_type_info* out)
{
    return guarded([&]() -> vt_status {
        const TypeDesc* type = typeOf(handle);
        if (!type) {
            return fail(VT_ERROR_NO_TYPE, "no type " + std::to_string(handle));
        }
        vt_type_info info{};
        info.name = type->name.c_str();
        info.description = type->description.c_str();
        info.property_count = static_cast<uint32_t>(type->properties.size());
        info.flags = (type->resolve ? uint32_t{VT_TYPE_ON_ENTITY} : 0u) |
            (type->isComponent() ? uint32_t{VT_TYPE_COMPONENT} : 0u);
        return fillSized(out, info);
    });
}

vt_status vt_property_find(const char* name, vt_property* out)
{
    return guarded([&]() -> vt_status {
        if (!name || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no name or no output");
        }
        const auto [type, property] = PropertyRegistry::instance().find(name);
        if (!type) {
            return fail(VT_ERROR_NO_TYPE, std::string("no type in '") + name + "' (type.property)");
        }
        if (!property) {
            return fail(VT_ERROR_NO_PROPERTY, std::string("no property ") + name);
        }
        const auto index = static_cast<uint64_t>(property - type->properties.data());
        *out = (static_cast<uint64_t>(handleOf(type)) << 32) | index;
        return VT_OK;
    });
}

vt_status vt_property_at(const vt_type handle, const uint32_t index, vt_property* out)
{
    return guarded([&]() -> vt_status {
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        const TypeDesc* type = typeOf(handle);
        if (!type) {
            return fail(VT_ERROR_NO_TYPE, "no type " + std::to_string(handle));
        }
        if (index >= type->properties.size()) {
            return fail(VT_ERROR_NO_PROPERTY, type->name + " has no property at index " + std::to_string(index));
        }
        *out = (static_cast<uint64_t>(handle) << 32) | index;
        return VT_OK;
    });
}

vt_status vt_property_get_info(const vt_property handle, vt_property_info* out)
{
    return guarded([&]() -> vt_status {
        const TypeDesc* owner = nullptr;
        const PropertyDesc* property = propertyOf(handle, &owner);
        if (!property) {
            return fail(VT_ERROR_NO_PROPERTY, "no property " + std::to_string(handle));
        }
        vt_property_info info{};
        info.name = property->name.c_str();
        info.description = property->description.c_str();
        info.unit = property->unit.c_str();
        info.value_type = valueTypeOf(property->type);
        info.flags = property->readOnly() ? uint32_t{VT_PROPERTY_READ_ONLY} : 0u;
        info.min = property->min;
        info.max = property->max;
        info.step = property->step;
        info.option_count = static_cast<uint32_t>(property->options.size());
        info.type = handleOf(owner);
        return fillSized(out, info);
    });
}

vt_status vt_property_option(const vt_property handle, const uint32_t index, int32_t* value, const char** name)
{
    return guarded([&]() -> vt_status {
        const PropertyDesc* property = propertyOf(handle);
        if (!property) {
            return fail(VT_ERROR_NO_PROPERTY, "no property " + std::to_string(handle));
        }
        if (index >= property->options.size()) {
            return fail(VT_ERROR_INVALID_ARGUMENT, property->name + " has no option at index " + std::to_string(index));
        }
        if (value) {
            *value = property->options[index].value;
        }
        if (name) {
            *name = property->options[index].name.c_str();
        }
        return VT_OK;
    });
}

// ---- Property values ------------------------------------------------------------------

vt_status vt_get_bool(vt_engine* e, const vt_entity id, const vt_property p, int32_t* out)
{
    return getTyped(e, id, p, ValueType::Bool, out, [](const PropertyValue& v) { return std::get<bool>(v) ? 1 : 0; });
}

vt_status vt_set_bool(vt_engine* e, const vt_entity id, const vt_property p, const int32_t value)
{
    return setTyped(e, id, p, ValueType::Bool, PropertyValue(value != 0));
}

vt_status vt_get_int(vt_engine* e, const vt_entity id, const vt_property p, int32_t* out)
{
    return getTyped(e, id, p, ValueType::Int, out, [](const PropertyValue& v) { return std::get<int>(v); });
}

vt_status vt_set_int(vt_engine* e, const vt_entity id, const vt_property p, const int32_t value)
{
    return setTyped(e, id, p, ValueType::Int, PropertyValue(static_cast<int>(value)));
}

vt_status vt_get_float(vt_engine* e, const vt_entity id, const vt_property p, float* out)
{
    return getTyped(e, id, p, ValueType::Float, out, [](const PropertyValue& v) { return std::get<float>(v); });
}

vt_status vt_set_float(vt_engine* e, const vt_entity id, const vt_property p, const float value)
{
    return setTyped(e, id, p, ValueType::Float, PropertyValue(value));
}

vt_status vt_get_vec2(vt_engine* e, const vt_entity id, const vt_property p, vt_vec2* out)
{
    return getTyped(e, id, p, ValueType::Vec2, out, [](const PropertyValue& v) {
        const auto& x = std::get<Vector2>(v);
        return vt_vec2{x.x, x.y};
    });
}

vt_status vt_set_vec2(vt_engine* e, const vt_entity id, const vt_property p, const vt_vec2 value)
{
    return setTyped(e, id, p, ValueType::Vec2, PropertyValue(Vector2(value.x, value.y)));
}

vt_status vt_get_vec3(vt_engine* e, const vt_entity id, const vt_property p, vt_vec3* out)
{
    return getTyped(e, id, p, ValueType::Vec3, out, [](const PropertyValue& v) {
        const auto& x = std::get<Vector3>(v);
        return vt_vec3{x.getX(), x.getY(), x.getZ()};
    });
}

vt_status vt_set_vec3(vt_engine* e, const vt_entity id, const vt_property p, const vt_vec3 value)
{
    return setTyped(e, id, p, ValueType::Vec3, PropertyValue(Vector3(value.x, value.y, value.z)));
}

vt_status vt_get_vec4(vt_engine* e, const vt_entity id, const vt_property p, vt_vec4* out)
{
    return getTyped(e, id, p, ValueType::Vec4, out, [](const PropertyValue& v) {
        const auto& x = std::get<Vector4>(v);
        return vt_vec4{x.getX(), x.getY(), x.getZ(), x.getW()};
    });
}

vt_status vt_set_vec4(vt_engine* e, const vt_entity id, const vt_property p, const vt_vec4 value)
{
    return setTyped(e, id, p, ValueType::Vec4, PropertyValue(Vector4(value.x, value.y, value.z, value.w)));
}

vt_status vt_get_quat(vt_engine* e, const vt_entity id, const vt_property p, vt_vec4* out)
{
    return getTyped(e, id, p, ValueType::Quat, out, [](const PropertyValue& v) {
        const auto& x = std::get<Quaternion>(v);
        return vt_vec4{x.getX(), x.getY(), x.getZ(), x.getW()};
    });
}

vt_status vt_set_quat(vt_engine* e, const vt_entity id, const vt_property p, const vt_vec4 value)
{
    return setTyped(e, id, p, ValueType::Quat, PropertyValue(Quaternion(value.x, value.y, value.z, value.w)));
}

vt_status vt_get_color(vt_engine* e, const vt_entity id, const vt_property p, vt_color* out)
{
    return getTyped(e, id, p, ValueType::Color, out, [](const PropertyValue& v) {
        const auto& c = std::get<Color>(v);
        return vt_color{c.r, c.g, c.b, c.a};
    });
}

vt_status vt_set_color(vt_engine* e, const vt_entity id, const vt_property p, const vt_color value)
{
    return setTyped(e, id, p, ValueType::Color, PropertyValue(Color(value.r, value.g, value.b, value.a)));
}

vt_status vt_get_entity(vt_engine* e, const vt_entity id, const vt_property p, vt_entity* out)
{
    return getTyped(e, id, p, ValueType::EntityRef, out,
        [](const PropertyValue& v) { return std::get<EntityId>(v).value; });
}

vt_status vt_set_entity(vt_engine* e, const vt_entity id, const vt_property p, const vt_entity value)
{
    return setTyped(e, id, p, ValueType::EntityRef, PropertyValue(EntityId{value}));
}

vt_status vt_get_string(vt_engine* e, const vt_entity id, const vt_property p, char* buffer, const size_t capacity,
    size_t* length)
{
    std::string text;
    if (const vt_status status = getTyped(e, id, p, ValueType::String, &text,
            [](const PropertyValue& v) { return std::get<std::string>(v); });
        status != VT_OK) {
        return status;
    }
    return guarded([&]() -> vt_status { return writeString(text, buffer, capacity, length); });
}

vt_status vt_set_string(vt_engine* e, const vt_entity id, const vt_property p, const char* value)
{
    if (!value) {
        return guarded([] { return fail(VT_ERROR_INVALID_ARGUMENT, "no string"); });
    }
    PropertyValue text;
    if (const vt_status status = guarded([&] {
            text = std::string(value);
            return VT_OK;
        });
        status != VT_OK) {
        return status;
    }
    return setTyped(e, id, p, ValueType::String, std::move(text));
}

vt_status vt_get_text(vt_engine* engine, const vt_entity id, const vt_property handle, char* buffer,
    const size_t capacity, size_t* length)
{
    return guarded([&]() -> vt_status {
        const PropertyDesc* property = nullptr;
        void* target = nullptr;
        if (const vt_status status = resolveProperty(engine, id, handle, property, target); status != VT_OK) {
            return status;
        }
        return writeString(formatPropertyValue(*property, property->get(target)), buffer, capacity, length);
    });
}

vt_status vt_set_text(vt_engine* engine, const vt_entity id, const vt_property handle, const char* text)
{
    return guarded([&]() -> vt_status {
        if (!text) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no text");
        }
        const PropertyDesc* property = nullptr;
        void* target = nullptr;
        const TypeDesc* owner = nullptr;
        if (const vt_status status = resolveProperty(engine, id, handle, property, target, &owner); status != VT_OK) {
            return status;
        }
        PropertyValue value;
        if (!parsePropertyValue(*property, text, value)) {
            return fail(VT_ERROR_TYPE_MISMATCH, std::string("'") + text + "' is not a " +
                valueTypeName(property->type) + " for " + qualifiedName(*owner, *property));
        }
        return fromPropertyStatus(PropertyRegistry::set(*property, target, value), qualifiedName(*owner, *property));
    });
}

} // extern "C"
