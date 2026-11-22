// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// The C API's shared helpers (capiInternal.h).
#include "capiInternal.h"

namespace visutwin::canvas::capi
{
    thread_local std::string lastError;

    vt_status fail(const vt_status status, std::string message)
    {
        lastError = std::move(message);
        return status;
    }

    // The status of an exception caught at the boundary. Building the message may itself
    // throw (out of memory); then the error text stays empty, and nothing escapes into C.
    vt_status failInternal(const char* what) noexcept
    {
        try {
            lastError = what ? std::string("engine error: ") + what : std::string("engine error");
        } catch (...) {
            lastError.clear();
        }
        return VT_ERROR_INTERNAL;
    }

    vt_status checkEngine(const vt_engine* engine)
    {
        if (!engine || !engine->engine) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no engine");
        }
        if (std::this_thread::get_id() != engine->thread) {
            return fail(VT_ERROR_WRONG_THREAD, "an engine is driven only from the thread that created it");
        }
        return VT_OK;
    }

    // The engine check, then the entity, or a status.
    vt_status findEntity(vt_engine* engine, const vt_entity id, Entity*& out)
    {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        out = engine->engine->entityById(EntityId{id});
        if (!out) {
            return fail(VT_ERROR_NOT_FOUND, "no entity " + std::to_string(id) + " in this engine");
        }
        return VT_OK;
    }

    // Writes `text` for the two-call pattern: no buffer only reports the length.
    vt_status writeString(const std::string_view text, char* buffer, const size_t capacity, size_t* length)
    {
        if (length) {
            *length = text.size();
        }
        if (!buffer) {
            return VT_OK;
        }
        if (capacity == 0) {
            return fail(VT_ERROR_BUFFER_TOO_SMALL, "a buffer of capacity 0");
        }
        const size_t copied = std::min(text.size(), capacity - 1);
        std::memcpy(buffer, text.data(), copied);
        buffer[copied] = '\0';
        if (copied < text.size()) {
            return fail(VT_ERROR_BUFFER_TOO_SMALL, "the text needs " + std::to_string(text.size() + 1) + " bytes");
        }
        return VT_OK;
    }

    vt_status fromPropertyStatus(const PropertyStatus status, const std::string_view what)
    {
        std::string message = std::string(what) + ": " + propertyStatusName(status);
        switch (status) {
        case PropertyStatus::Ok: return VT_OK;
        case PropertyStatus::NoType: return fail(VT_ERROR_NO_TYPE, std::move(message));
        case PropertyStatus::NoProperty: return fail(VT_ERROR_NO_PROPERTY, std::move(message));
        case PropertyStatus::NoTarget: return fail(VT_ERROR_NO_TARGET, std::move(message));
        case PropertyStatus::ReadOnly: return fail(VT_ERROR_READ_ONLY, std::move(message));
        case PropertyStatus::TypeMismatch: return fail(VT_ERROR_TYPE_MISMATCH, std::move(message));
        case PropertyStatus::InvalidValue: return fail(VT_ERROR_INVALID_VALUE, std::move(message));
        }
        return fail(VT_ERROR_INTERNAL, std::move(message));
    }

    // ---- Schema handles: a type is its index + 1, a property (type << 32) | its index ----

    const TypeDesc* typeOf(const vt_type type)
    {
        const auto& types = PropertyRegistry::instance().types();
        return type >= 1 && type <= types.size() ? types[type - 1].get() : nullptr;
    }

    vt_type handleOf(const TypeDesc* type)
    {
        const auto& types = PropertyRegistry::instance().types();
        for (size_t i = 0; i < types.size(); ++i) {
            if (types[i].get() == type) {
                return static_cast<vt_type>(i + 1);
            }
        }
        return 0;
    }

    const PropertyDesc* propertyOf(const vt_property property, const TypeDesc** owner)
    {
        const TypeDesc* type = typeOf(static_cast<vt_type>(property >> 32));
        const uint64_t index = property & 0xffffffffu;
        if (!type || index >= type->properties.size()) {
            return nullptr;
        }
        if (owner) {
            *owner = type;
        }
        return &type->properties[index];
    }

    vt_value_type valueTypeOf(const ValueType type)
    {
        switch (type) {
        case ValueType::Bool: return VT_VALUE_BOOL;
        case ValueType::Int: return VT_VALUE_INT;
        case ValueType::Float: return VT_VALUE_FLOAT;
        case ValueType::Vec2: return VT_VALUE_VEC2;
        case ValueType::Vec3: return VT_VALUE_VEC3;
        case ValueType::Vec4: return VT_VALUE_VEC4;
        case ValueType::Quat: return VT_VALUE_QUAT;
        case ValueType::Color: return VT_VALUE_COLOR;
        case ValueType::String: return VT_VALUE_STRING;
        case ValueType::Enum: return VT_VALUE_ENUM;
        case ValueType::EntityRef: return VT_VALUE_ENTITY;
        }
        return VT_VALUE_NONE;
    }

    std::string qualifiedName(const TypeDesc& type, const PropertyDesc& property)
    {
        return type.name + "." + property.name;
    }

    // The property's object on the entity, after the engine, entity and property checks.
    vt_status resolveProperty(vt_engine* engine, const vt_entity id, const vt_property handle,
        const PropertyDesc*& property, void*& target, const TypeDesc** ownerOut)
    {
        if (id & VT_RESOURCE_BIT) {
            if (const vt_status status = checkEngine(engine); status != VT_OK) {
                return status;
            }
            const auto it = engine->resources.find(id);
            if (it == engine->resources.end()) {
                return fail(VT_ERROR_NOT_FOUND, "no resource " + std::to_string(id & ~VT_RESOURCE_BIT) +
                    " in this engine");
            }
            const TypeDesc* owner = nullptr;
            property = propertyOf(handle, &owner);
            if (!property) {
                return fail(VT_ERROR_NO_PROPERTY, "no property " + std::to_string(handle));
            }
            if (owner != it->second.type) {
                return fail(VT_ERROR_NO_TARGET, "this resource has no " + owner->name + " properties");
            }
            target = it->second.target;
            if (ownerOut) {
                *ownerOut = owner;
            }
            return VT_OK;
        }
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        const TypeDesc* owner = nullptr;
        property = propertyOf(handle, &owner);
        if (!property) {
            return fail(VT_ERROR_NO_PROPERTY, "no property " + std::to_string(handle));
        }
        if (!owner->resolve) {
            return fail(VT_ERROR_NO_TARGET, "type " + owner->name + " is not carried by an entity");
        }
        target = owner->resolve(*entity);
        if (!target) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no " + owner->name);
        }
        if (ownerOut) {
            *ownerOut = owner;
        }
        return VT_OK;
    }

    bool typeAccepts(const ValueType actual, const ValueType asked)
    {
        return actual == asked || (asked == ValueType::Int && actual == ValueType::Enum);
    }

    vt_status setTyped(vt_engine* engine, const vt_entity id, const vt_property handle, const ValueType asked,
        const PropertyValue& value)
    {
        return guarded([&]() -> vt_status {
            const PropertyDesc* property = nullptr;
            void* target = nullptr;
            const TypeDesc* owner = nullptr;
            if (const vt_status status = resolveProperty(engine, id, handle, property, target, &owner);
                status != VT_OK) {
                return status;
            }
            if (!typeAccepts(property->type, asked)) {
                return fail(VT_ERROR_TYPE_MISMATCH, qualifiedName(*owner, *property) + " is a " +
                    valueTypeName(property->type) + ", not a " + valueTypeName(asked));
            }
            return fromPropertyStatus(PropertyRegistry::set(*property, target, value),
                qualifiedName(*owner, *property));
        });
    }

    const TypeDesc* componentType(const char* name, vt_status& status)
    {
        if (!name) {
            status = fail(VT_ERROR_INVALID_ARGUMENT, "no type name");
            return nullptr;
        }
        const TypeDesc* type = PropertyRegistry::instance().type(name);
        if (!type) {
            status = fail(VT_ERROR_NO_TYPE, std::string("no type ") + name);
            return nullptr;
        }
        if (!type->isComponent()) {
            status = fail(VT_ERROR_INVALID_ARGUMENT, std::string(name) + " is not a component type");
            return nullptr;
        }
        status = VT_OK;
        return type;
    }

    void countFrame(vt_engine* engine) { ++engine->frames; }

    vt_resource addResource(vt_engine* engine, vt_engine::ResourceKind kind, const char* typeName,
        std::shared_ptr<void> owner, void* target)
    {
        const vt_resource id = VT_RESOURCE_BIT | engine->nextResource++;
        engine->resources[id] = {kind, typeName ? PropertyRegistry::instance().type(typeName) : nullptr,
            std::move(owner), target};
        return id;
    }

    // The resource of a kind, after the engine check, or a status.
    vt_status findResource(vt_engine* engine, const vt_resource id, const vt_engine::ResourceKind kind,
        const char* kindName, vt_engine::Resource*& out)
    {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        const auto it = engine->resources.find(id);
        if (!(id & VT_RESOURCE_BIT) || it == engine->resources.end()) {
            return fail(VT_ERROR_NOT_FOUND, "no resource " + std::to_string(id & ~VT_RESOURCE_BIT) + " in this engine");
        }
        if (it->second.kind != kind) {
            return fail(VT_ERROR_INVALID_ARGUMENT, std::string("the resource is not a ") + kindName);
        }
        out = &it->second;
        return VT_OK;
    }

    uint32_t bytesPerPixel(const vt_pixel_format format)
    {
        switch (format) {
        case VT_PIXEL_RGBA8: return 4;
        case VT_PIXEL_RGBA16F: return 8;
        case VT_PIXEL_RGBA32F: return 16;
        default: return 0;
        }
    }

    // The vertices of a mesh desc over given triangles: normals derived when absent, tangents
    // when there are uvs, zero uvs otherwise.
    vt_status vertexGeometry(const vt_mesh_desc& d, const std::vector<uint32_t>& indices, PrimitiveGeometry& geometry)
    {
        const size_t n = d.vertex_count;
        geometry.positions.assign(d.positions, d.positions + n * 3);
        for (const float value : geometry.positions) {
            if (!std::isfinite(value)) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "a position is not finite");
            }
        }
        geometry.indices = indices;
        geometry.normals = d.normals ? std::vector<float>(d.normals, d.normals + n * 3)
                                     : calculateNormals(geometry.positions, geometry.indices);
        if (d.uvs) {
            geometry.uvs.assign(d.uvs, d.uvs + n * 2);
            geometry.tangents = calculateTangents(geometry.positions, geometry.normals, geometry.uvs, geometry.indices);
        } else {
            geometry.uvs.assign(n * 2, 0.0f);
        }
        return VT_OK;
    }

    void startIfNeeded(vt_engine* engine)
    {
        if (!engine->started) {
            engine->started = true;
            engine->engine->start();
            countFrame(engine);
        }
    }

    void teardown(vt_engine* engine)
    {
        // The engine owns entities that borrow the device: it goes first, SDL last.
        const std::weak_ptr<GraphicsDevice> device = engine->device;
        // destroy() and not only the reset: the engine's script registry, element input and tick
        // hold the engine itself, so without it the last reference dropped here frees nothing.
        // The picker draws through the engine: it goes first.
        engine->picker.reset();
        if (engine->renderRemoved) {
            engine->renderRemoved->off();
            engine->cameraRemoved->off();
        }
        if (engine->engine) {
            engine->engine->destroy();
        }
        engine->engine.reset();
        // The scene is gone with the engine; the caller's resources go before the device.
        engine->offscreen.clear();
        engine->environment.reset();
        engine->renderMaterials.clear();
        engine->resources.clear();
        engine->elementInput.reset();
        engine->device.reset();
        // Everything holding the device has let go by now; a device kept alive past this point
        // (a static cache, a cycle) would be freed at process exit, after its driver.
        if (const auto survivor = device.lock()) {
            spdlog::warn("vt_engine_destroy: the graphics device outlived its engine ({} references)",
                survivor.use_count() - 1);
        }
        if (engine->renderer) {
            SDL_DestroyRenderer(engine->renderer);
            engine->renderer = nullptr;
        }
        if (engine->window) {
            SDL_DestroyWindow(engine->window);
            engine->window = nullptr;
        }
        if (engine->gamepadInitialized) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
            engine->gamepadInitialized = false;
        }
        if (engine->sdlInitialized) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            engine->sdlInitialized = false;
        }
    }
}
