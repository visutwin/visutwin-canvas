// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// The C API over the engine. Every entry point clears the thread's error, checks its
// arguments and the engine's thread, and catches whatever the engine throws, so no exception
// crosses the boundary. Entities are found by id on each call; properties go through the
// property table (framework/properties), so a property registered there is reachable here
// without a line of this file.
//
#include "visutwin/visutwin.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>
#include <array>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/assets/asset.h"
#include "framework/graphics/picker.h"
#include "framework/handlers/containerResource.h"
#include "framework/input/elementInput.h"
#include "framework/properties/propertyTable.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/null/nullGraphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/geometry/geometryUtils.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/scene.h"
#include "spdlog/spdlog.h"

using namespace visutwin::canvas;

struct vt_engine
{
    std::thread::id thread;
    vt_backend backend = VT_BACKEND_NULL;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool sdlInitialized = false;
    bool gamepadInitialized = false;
    std::shared_ptr<GraphicsDevice> device;
    std::shared_ptr<ElementInput> elementInput;
    std::shared_ptr<Engine> engine;
    bool started = false;
    uint64_t frames = 0;

    // Resources by id (VT_RESOURCE_BIT set). The record holds the caller's reference; whatever
    // the scene still uses co-owns the object and keeps it alive past a release.
    enum class ResourceKind { Scene, Material, Mesh, Texture };
    struct Resource
    {
        ResourceKind kind;
        const visutwin::canvas::TypeDesc* type = nullptr;   // its properties' type, or none
        std::shared_ptr<void> owner;                        // null for the scene, which the engine owns
        void* target = nullptr;
        // A mesh's triangles, which an update of its vertices keeps.
        std::vector<uint32_t> indices;
        uint32_t vertexCount = 0;
    };
    std::unordered_map<uint64_t, Resource> resources;
    uint64_t nextResource = 1;
    uint64_t sceneResource = 0;

    // Per render entity: the material it was given, which a primitive rebuilt later (a changed
    // render.type) takes by raw pointer, so it must stay alive while the entity has it.
    std::unordered_map<uint64_t, std::shared_ptr<visutwin::canvas::Material>> renderMaterials;

    // Per offscreen camera: the image it renders into.
    struct Offscreen
    {
        std::shared_ptr<visutwin::canvas::Texture> texture;
        std::shared_ptr<visutwin::canvas::RenderTarget> target;
    };
    std::unordered_map<uint64_t, Offscreen> offscreen;

    // The environment atlas the scene lights with, which it holds by raw pointer.
    std::shared_ptr<void> environment;

    // The window's mouse and keys as the SDL events of the last poll left them (the engine's
    // input devices end their press edges inside the update, before a caller could read them).
    vt_input_state input{sizeof(vt_input_state), 0.0f, 0.0f, 0u, 0u, 0u, 0.0f, 0.0f, 0};
    std::array<uint8_t, SDL_SCANCODE_COUNT> keysDown{};
    std::array<uint8_t, SDL_SCANCODE_COUNT> keysPressed{};

    // Made on the first pick.
    std::unique_ptr<visutwin::canvas::Picker> picker;

    // Extensions' callbacks, run when the engine is destroyed (vt_engine_on_destroy).
    std::vector<std::pair<vt_engine_destroy_callback, void*>> onDestroy;
};

namespace
{
    thread_local std::string lastError;

    vt_status fail(const vt_status status, std::string message)
    {
        lastError = std::move(message);
        return status;
    }

    // Runs one entry point: a clear error to start, every exception caught.
    template <class Body>
    vt_status guarded(Body&& body)
    {
        lastError.clear();
        try {
            return body();
        } catch (const std::exception& exception) {
            return fail(VT_ERROR_INTERNAL, std::string("engine error: ") + exception.what());
        } catch (...) {
            return fail(VT_ERROR_INTERNAL, "engine error");
        }
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

    const PropertyDesc* propertyOf(const vt_property property, const TypeDesc** owner = nullptr)
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

    // Copies a struct the library filled into the caller's, as far as the caller's size says.
    template <class T>
    vt_status fillSized(T* out, T filled)
    {
        if (!out || out->size < sizeof(uint32_t)) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "an output struct without its size");
        }
        const uint32_t size = out->size;
        filled.size = size;
        std::memcpy(out, &filled, std::min<size_t>(size, sizeof(T)));
        return VT_OK;
    }

    std::string qualifiedName(const TypeDesc& type, const PropertyDesc& property)
    {
        return type.name + "." + property.name;
    }

    // The property's object on the entity, after the engine, entity and property checks.
    vt_status resolveProperty(vt_engine* engine, const vt_entity id, const vt_property handle,
        const PropertyDesc*& property, void*& target, const TypeDesc** ownerOut = nullptr)
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

    template <class Out, class Convert>
    vt_status getTyped(vt_engine* engine, const vt_entity id, const vt_property handle, const ValueType asked,
        Out* out, Convert convert)
    {
        return guarded([&]() -> vt_status {
            if (!out) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
            }
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
            *out = convert(property->get(target));
            return VT_OK;
        });
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

    // Every entity id of a subtree, the root of it included.
    void collectSubtree(GraphNode* node, std::vector<uint64_t>& ids)
    {
        if (const auto* entity = dynamic_cast<const Entity*>(node)) {
            ids.push_back(entity->id().value);
        }
        node->forEachChild([&](GraphNode* child) { collectSubtree(child, ids); });
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

extern "C" {

uint32_t vt_api_version(void)
{
    return (static_cast<uint32_t>(VT_API_VERSION_MAJOR) << 16) | VT_API_VERSION_MINOR;
}

const char* vt_status_name(const vt_status status)
{
    switch (status) {
    case VT_OK: return "VT_OK";
    case VT_ERROR_INVALID_ARGUMENT: return "VT_ERROR_INVALID_ARGUMENT";
    case VT_ERROR_INVALID_STATE: return "VT_ERROR_INVALID_STATE";
    case VT_ERROR_WRONG_THREAD: return "VT_ERROR_WRONG_THREAD";
    case VT_ERROR_NOT_FOUND: return "VT_ERROR_NOT_FOUND";
    case VT_ERROR_NO_TYPE: return "VT_ERROR_NO_TYPE";
    case VT_ERROR_NO_PROPERTY: return "VT_ERROR_NO_PROPERTY";
    case VT_ERROR_NO_TARGET: return "VT_ERROR_NO_TARGET";
    case VT_ERROR_READ_ONLY: return "VT_ERROR_READ_ONLY";
    case VT_ERROR_TYPE_MISMATCH: return "VT_ERROR_TYPE_MISMATCH";
    case VT_ERROR_INVALID_VALUE: return "VT_ERROR_INVALID_VALUE";
    case VT_ERROR_BUFFER_TOO_SMALL: return "VT_ERROR_BUFFER_TOO_SMALL";
    case VT_ERROR_UNSUPPORTED: return "VT_ERROR_UNSUPPORTED";
    case VT_ERROR_INTERNAL: return "VT_ERROR_INTERNAL";
    default: return "VT_ERROR_UNKNOWN";
    }
}

const char* vt_last_error(void)
{
    return lastError.c_str();
}

// ---- Engine ---------------------------------------------------------------------------

vt_status vt_engine_create(const vt_engine_desc* desc, vt_engine** out)
{
    return guarded([&]() -> vt_status {
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = nullptr;
        // The fields this caller's header knows; the rest keep their defaults.
        vt_engine_desc d{};
        d.size = sizeof(vt_engine_desc);
        if (desc) {
            if (desc->size < sizeof(uint32_t)) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "vt_engine_desc.size is not set");
            }
            std::memcpy(&d, desc, std::min<size_t>(desc->size, sizeof(vt_engine_desc)));
        }
        const int width = d.width > 0 ? d.width : 1280;
        const int height = d.height > 0 ? d.height : 720;
        if (d.backend < VT_BACKEND_DEFAULT || d.backend > VT_BACKEND_NULL) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "unknown backend " + std::to_string(d.backend));
        }

        auto engine = std::make_unique<vt_engine>();
        engine->thread = std::this_thread::get_id();

        if (d.backend == VT_BACKEND_NULL) {
            engine->backend = VT_BACKEND_NULL;
            engine->device = std::make_shared<NullGraphicsDevice>(width, height);
        } else {
            Backend backend = defaultBackend();
            if (d.backend == VT_BACKEND_METAL) {
                backend = Backend::Metal;
            } else if (d.backend == VT_BACKEND_VULKAN) {
                backend = Backend::Vulkan;
            }
            engine->backend = backend == Backend::Metal ? VT_BACKEND_METAL : VT_BACKEND_VULKAN;
            if (backend == Backend::Metal) {
                SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
            }
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
                return fail(VT_ERROR_UNSUPPORTED, std::string("SDL could not start: ") + SDL_GetError());
            }
            engine->sdlInitialized = true;
            // Without it SDL delivers no gamepad events; the engine runs on without them.
            engine->gamepadInitialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
            SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
            if (d.flags & VT_ENGINE_HIDDEN) {
                flags |= SDL_WINDOW_HIDDEN;
            }
            flags |= backend == Backend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL;
            engine->window = SDL_CreateWindow(d.title ? d.title : "VisuTwin", width, height, flags);
            if (!engine->window) {
                const std::string error = SDL_GetError();
                teardown(engine.get());
                return fail(VT_ERROR_UNSUPPORTED, "no window: " + error);
            }

            GraphicsDeviceOptions options;
            options.backend = backend;
            options.window = engine->window;
            options.antialias = (d.flags & VT_ENGINE_NO_ANTIALIAS) == 0;
            if (backend == Backend::Metal) {
                // The SDL renderer only hands over its CAMetalLayer.
                engine->renderer = SDL_CreateRenderer(engine->window, nullptr);
                options.swapChain = engine->renderer ? SDL_GetRenderMetalLayer(engine->renderer) : nullptr;
                options.vsync = (d.flags & VT_ENGINE_NO_VSYNC) == 0;
                if (!options.swapChain) {
                    const std::string error = SDL_GetError();
                    teardown(engine.get());
                    return fail(VT_ERROR_UNSUPPORTED, "no Metal layer: " + error);
                }
                SDL_SetRenderVSync(engine->renderer, options.vsync ? SDL_RENDERER_VSYNC_ADAPTIVE : 0);
            }
            engine->device = createGraphicsDevice(options);
            if (!engine->device) {
                teardown(engine.get());
                return fail(VT_ERROR_UNSUPPORTED, std::string("no ") + backendName(backend) + " device");
            }
        }

        AppOptions appOptions;
        appOptions.graphicsDevice = engine->device;
        appOptions.keyboard = std::make_shared<Keyboard>();
        appOptions.mouse = std::make_shared<Mouse>();
        appOptions.touch = std::make_shared<TouchDevice>();
        appOptions.gamepads = std::make_shared<GamePads>();
        appOptions.registerComponentSystem<RenderComponentSystem>();
        appOptions.registerComponentSystem<CameraComponentSystem>();
        appOptions.registerComponentSystem<LightComponentSystem>();
        appOptions.registerComponentSystem<ScriptComponentSystem>();
        appOptions.registerComponentSystem<ScreenComponentSystem>();
        appOptions.registerComponentSystem<ElementComponentSystem>();
        engine->elementInput = std::make_shared<ElementInput>();
        appOptions.elementInput = engine->elementInput;

        engine->engine = std::make_shared<Engine>(engine->window);
        engine->engine->init(appOptions);
        if (engine->window) {
            engine->engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
            engine->engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
        }
        engine->sceneResource = addResource(engine.get(), vt_engine::ResourceKind::Scene, "scene", nullptr,
            engine->engine->scene().get());
        *out = engine.release();
        return VT_OK;
    });
}

vt_status vt_engine_destroy(vt_engine* engine)
{
    return guarded([&]() -> vt_status {
        if (!engine) {
            return VT_OK;
        }
        if (std::this_thread::get_id() != engine->thread) {
            return fail(VT_ERROR_WRONG_THREAD, "an engine is destroyed on the thread that created it");
        }
        // Extensions first, while everything they may hold on to still exists.
        for (const auto& [callback, user] : engine->onDestroy) {
            callback(engine, user);
        }
        engine->onDestroy.clear();
        teardown(engine);
        delete engine;
        return VT_OK;
    });
}

vt_status vt_engine_backend(vt_engine* engine, vt_backend* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = engine->backend;
        return VT_OK;
    });
}

vt_status vt_engine_start(vt_engine* engine)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (engine->started) {
            return fail(VT_ERROR_INVALID_STATE, "the engine has started already");
        }
        startIfNeeded(engine);
        return VT_OK;
    });
}

vt_status vt_poll_events(vt_engine* engine, int32_t* quit)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        bool closing = false;
        vt_input_state& input = engine->input;
        input.pressed = input.released = 0u;
        input.wheel_x = input.wheel_y = 0.0f;
        engine->keysPressed.fill(0);
        if (engine->window) {
            const auto buttonBit = [](const uint8_t button) -> uint32_t {
                return button == SDL_BUTTON_LEFT ? uint32_t{VT_MOUSE_LEFT}
                     : button == SDL_BUTTON_MIDDLE ? uint32_t{VT_MOUSE_MIDDLE}
                     : button == SDL_BUTTON_RIGHT ? uint32_t{VT_MOUSE_RIGHT} : 0u;
            };
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                engine->engine->handleInputEvent(event);
                switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    closing = true;
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    input.mouse_x = event.motion.x;
                    input.mouse_y = event.motion.y;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    input.mouse_x = event.button.x;
                    input.mouse_y = event.button.y;
                    input.buttons |= buttonBit(event.button.button);
                    input.pressed |= buttonBit(event.button.button);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    input.mouse_x = event.button.x;
                    input.mouse_y = event.button.y;
                    input.buttons &= ~buttonBit(event.button.button);
                    input.released |= buttonBit(event.button.button);
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    input.wheel_x += event.wheel.x;
                    input.wheel_y += event.wheel.y;
                    break;
                case SDL_EVENT_WINDOW_MOUSE_ENTER:
                    input.inside = 1;
                    break;
                case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                    input.inside = 0;
                    break;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    // A button or key held while focus moved away never sends its release.
                    input.released |= input.buttons;
                    input.buttons = 0u;
                    engine->keysDown.fill(0);
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.scancode < SDL_SCANCODE_COUNT) {
                        if (!event.key.repeat) {
                            engine->keysPressed[event.key.scancode] = 1;
                        }
                        engine->keysDown[event.key.scancode] = 1;
                    }
                    break;
                case SDL_EVENT_KEY_UP:
                    if (event.key.scancode < SDL_SCANCODE_COUNT) {
                        engine->keysDown[event.key.scancode] = 0;
                    }
                    break;
                default:
                    break;
                }
            }
        }
        if (quit) {
            *quit = closing ? 1 : 0;
        }
        return VT_OK;
    });
}

vt_status vt_wait_next_frame(vt_engine* engine)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        engine->engine->waitForNextFrame();
        return VT_OK;
    });
}

vt_status vt_update(vt_engine* engine, const float dt)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!std::isfinite(dt) || dt < 0.0f) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "dt must be a finite, non-negative number of seconds");
        }
        if (!engine->started) {
            startIfNeeded(engine);
            return VT_OK;
        }
        engine->engine->update(dt);
        return VT_OK;
    });
}

vt_status vt_render(vt_engine* engine)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!engine->started) {
            startIfNeeded(engine);
            return VT_OK;
        }
        engine->engine->render();
        countFrame(engine);
        return VT_OK;
    });
}

vt_status vt_frame(vt_engine* engine, const float dt, int32_t* quit)
{
    if (const vt_status status = vt_poll_events(engine, quit); status != VT_OK) {
        return status;
    }
    if (const vt_status status = vt_wait_next_frame(engine); status != VT_OK) {
        return status;
    }
    if (const bool starting = engine && !engine->started; starting) {
        return vt_update(engine, dt);
    }
    if (const vt_status status = vt_update(engine, dt); status != VT_OK) {
        return status;
    }
    return vt_render(engine);
}

vt_status vt_frame_stats_get(vt_engine* engine, vt_frame_stats* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        vt_frame_stats stats{};
        stats.frames = engine->frames;
        if (const auto& application = engine->engine->stats()) {
            stats.draw_calls = application->drawCalls().total;
            stats.triangles = application->frame().triangles;
            stats.cameras = application->frame().cameras;
        }
        return fillSized(out, stats);
    });
}

// ---- Entities -------------------------------------------------------------------------

vt_status vt_entity_root(vt_engine* engine, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = engine->engine->root()->id().value;
        return VT_OK;
    });
}

vt_status vt_entity_create(vt_engine* engine, const vt_entity parent, const char* name, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        Entity* parentEntity = engine->engine->root();
        if (parent != 0) {
            if (const vt_status status = findEntity(engine, parent, parentEntity); status != VT_OK) {
                return status;
            }
        }
        auto entity = std::make_unique<Entity>();
        entity->setEngine(engine->engine.get());
        entity->setName(name ? name : "");
        Entity* created = entity.get();
        parentEntity->addChild(std::move(entity));
        if (out) {
            *out = created->id().value;
        }
        return VT_OK;
    });
}

vt_status vt_entity_destroy(vt_engine* engine, const vt_entity id)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (entity == engine->engine->root() || !entity->parent()) {
            return fail(VT_ERROR_INVALID_STATE, "the root cannot be destroyed");
        }
        std::vector<uint64_t> subtree;
        collectSubtree(entity, subtree);
        for (const uint64_t gone : subtree) {
            engine->renderMaterials.erase(gone);
            engine->offscreen.erase(gone);
        }
        entity->destroy();
        // The parent owned it; taking it back frees it and everything below it.
        std::unique_ptr<GraphNode> owned = entity->parent()->removeChild(entity);
        owned.reset();
        return VT_OK;
    });
}

vt_status vt_entity_alive(vt_engine* engine, const vt_entity id, int32_t* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = engine->engine->entityById(EntityId{id}) ? 1 : 0;
        return VT_OK;
    });
}

vt_status vt_entity_find(vt_engine* engine, const char* path, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!path || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no path or no output");
        }
        Entity* entity = engine->engine->entityByPath(path);
        if (!entity) {
            return fail(VT_ERROR_NOT_FOUND, std::string("no entity at '") + path + "'");
        }
        *out = entity->id().value;
        return VT_OK;
    });
}

vt_status vt_entity_path(vt_engine* engine, const vt_entity id, char* buffer, const size_t capacity, size_t* length)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        return writeString(entity->path(), buffer, capacity, length);
    });
}

vt_status vt_entity_parent(vt_engine* engine, const vt_entity id, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        const auto* parent = dynamic_cast<const Entity*>(entity->parent());
        *out = parent ? parent->id().value : 0;
        return VT_OK;
    });
}

vt_status vt_entity_set_parent(vt_engine* engine, const vt_entity id, const vt_entity parent)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        Entity* newParent = engine->engine->root();
        if (parent != 0) {
            if (const vt_status status = findEntity(engine, parent, newParent); status != VT_OK) {
                return status;
            }
        }
        if (!entity->parent()) {
            return fail(VT_ERROR_INVALID_STATE, "the root has no parent to change");
        }
        for (const GraphNode* node = newParent; node; node = node->parent()) {
            if (node == entity) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "an entity cannot move below itself");
            }
        }
        if (entity->parent() != newParent) {
            newParent->addChild(static_cast<GraphNode*>(entity));
        }
        return VT_OK;
    });
}

vt_status vt_entity_children(vt_engine* engine, const vt_entity id, vt_entity* buffer, const size_t capacity,
    size_t* count)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        std::vector<vt_entity> children;
        entity->forEachChild([&](GraphNode* child) {
            if (const auto* childEntity = dynamic_cast<const Entity*>(child)) {
                children.push_back(childEntity->id().value);
            }
        });
        if (count) {
            *count = children.size();
        }
        if (!buffer) {
            return VT_OK;
        }
        const size_t written = std::min(capacity, children.size());
        std::memcpy(buffer, children.data(), written * sizeof(vt_entity));
        if (written < children.size()) {
            return fail(VT_ERROR_BUFFER_TOO_SMALL, "the entity has " + std::to_string(children.size()) + " children");
        }
        return VT_OK;
    });
}

vt_status vt_entity_describe(vt_engine* engine, const vt_entity id, char* buffer, const size_t capacity,
    size_t* length)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        return writeString(describeEntityProperties(*entity), buffer, capacity, length);
    });
}

// ---- Components -----------------------------------------------------------------------

vt_status vt_component_add(vt_engine* engine, const vt_entity id, const char* typeName)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        vt_status status = VT_OK;
        const TypeDesc* type = componentType(typeName, status);
        if (!type) {
            return status;
        }
        if (!type->addTo(*entity)) {
            return fail(VT_ERROR_UNSUPPORTED, "entity " + std::to_string(id) + " cannot take a " + type->name +
                (type->name == "cameraControls" ? " (it needs a camera)" : ""));
        }
        return VT_OK;
    });
}

vt_status vt_component_remove(vt_engine* engine, const vt_entity id, const char* typeName)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        vt_status status = VT_OK;
        const TypeDesc* type = componentType(typeName, status);
        if (!type) {
            return status;
        }
        if (!type->removeFrom(*entity)) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no " + type->name);
        }
        return VT_OK;
    });
}

vt_status vt_component_has(vt_engine* engine, const vt_entity id, const char* typeName, int32_t* out)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        vt_status status = VT_OK;
        const TypeDesc* type = componentType(typeName, status);
        if (!type) {
            return status;
        }
        *out = type->resolve(*entity) ? 1 : 0;
        return VT_OK;
    });
}

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
        lastError = "no string";
        return VT_ERROR_INVALID_ARGUMENT;
    }
    return setTyped(e, id, p, ValueType::String, PropertyValue(std::string(value)));
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

// ---- Resources ------------------------------------------------------------------------

vt_status vt_engine_scene(vt_engine* engine, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = engine->sceneResource;
        return VT_OK;
    });
}

vt_status vt_material_create(vt_engine* engine, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        auto material = std::make_shared<StandardMaterial>();
        void* target = material.get();
        *out = addResource(engine, vt_engine::ResourceKind::Material, "standard", std::move(material), target);
        return VT_OK;
    });
}

vt_status vt_mesh_create(vt_engine* engine, const vt_mesh_desc* desc, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out || !desc || desc->size < sizeof(uint32_t)) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no desc, no output or vt_mesh_desc.size not set");
        }
        vt_mesh_desc d{};
        std::memcpy(&d, desc, std::min<size_t>(desc->size, sizeof(vt_mesh_desc)));
        if (!d.positions || d.vertex_count < 3) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "a mesh needs positions for at least 3 vertices");
        }
        const size_t n = d.vertex_count;
        std::vector<uint32_t> indices;
        if (d.index_count == 0) {
            if (n % 3 != 0) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "without indices the vertex count must be a multiple of 3");
            }
            indices.resize(n);
            for (size_t i = 0; i < n; ++i) {
                indices[i] = static_cast<uint32_t>(i);
            }
        } else {
            if (!d.indices || d.index_count % 3 != 0) {
                return fail(VT_ERROR_INVALID_ARGUMENT, "the index count must be a multiple of 3, with indices given");
            }
            indices.assign(d.indices, d.indices + d.index_count);
            for (const uint32_t index : indices) {
                if (index >= n) {
                    return fail(VT_ERROR_INVALID_ARGUMENT, "index " + std::to_string(index) + " is past the " +
                        std::to_string(n) + " vertices");
                }
            }
        }
        PrimitiveGeometry geometry;
        if (const vt_status status = vertexGeometry(d, indices, geometry); status != VT_OK) {
            return status;
        }
        std::shared_ptr<Mesh> mesh = createMeshFromGeometry(engine->device, geometry);
        if (!mesh) {
            return fail(VT_ERROR_INTERNAL, "the mesh could not be built");
        }
        void* target = mesh.get();
        *out = addResource(engine, vt_engine::ResourceKind::Mesh, nullptr, std::move(mesh), target);
        vt_engine::Resource& record = engine->resources[*out];
        record.indices = std::move(geometry.indices);
        record.vertexCount = d.vertex_count;
        return VT_OK;
    });
}

vt_status vt_mesh_update(vt_engine* engine, const vt_resource id, const vt_mesh_desc* desc)
{
    return guarded([&]() -> vt_status {
        vt_engine::Resource* resource = nullptr;
        if (const vt_status status = findResource(engine, id, vt_engine::ResourceKind::Mesh, "mesh", resource);
            status != VT_OK) {
            return status;
        }
        if (!desc || desc->size < sizeof(uint32_t)) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no desc or vt_mesh_desc.size not set");
        }
        vt_mesh_desc d{};
        std::memcpy(&d, desc, std::min<size_t>(desc->size, sizeof(vt_mesh_desc)));
        if (!d.positions || d.vertex_count != resource->vertexCount) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "an update gives positions for the mesh's " +
                std::to_string(resource->vertexCount) + " vertices");
        }
        PrimitiveGeometry geometry;
        if (const vt_status status = vertexGeometry(d, resource->indices, geometry); status != VT_OK) {
            return status;
        }
        const std::shared_ptr<Mesh> rebuilt = createMeshFromGeometry(engine->device, geometry);
        if (!rebuilt) {
            return fail(VT_ERROR_INTERNAL, "the mesh could not be rebuilt");
        }
        // A new vertex buffer into the same mesh, so every instance drawing it follows; the old
        // buffer is freed once the frames still drawing it are done (both backends defer it).
        auto* mesh = static_cast<Mesh*>(resource->target);
        mesh->setVertexBuffer(rebuilt->getVertexBuffer());
        mesh->setAabb(rebuilt->aabb());
        return VT_OK;
    });
}

vt_status vt_texture_create(vt_engine* engine, const vt_texture_desc* desc, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out || !desc || desc->size < sizeof(uint32_t)) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no desc, no output or vt_texture_desc.size not set");
        }
        vt_texture_desc d{};
        std::memcpy(&d, desc, std::min<size_t>(desc->size, sizeof(vt_texture_desc)));
        const uint32_t pixelBytes = bytesPerPixel(d.format);
        if (pixelBytes == 0) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "unknown pixel format " + std::to_string(d.format));
        }
        const auto maxSize = static_cast<uint32_t>(engine->device->maxTextureSize());
        if (d.width == 0 || d.height == 0 || d.width > maxSize || d.height > maxSize) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "a texture is 1 to " + std::to_string(maxSize) + " pixels a side");
        }
        const size_t expected = size_t(d.width) * d.height * pixelBytes;
        if (d.data && d.data_size != expected) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "the pixels are " + std::to_string(d.data_size) + " bytes, not " +
                std::to_string(expected));
        }
        TextureOptions options;
        options.width = d.width;
        options.height = d.height;
        options.format = d.format == VT_PIXEL_RGBA8 ? PixelFormat::PIXELFORMAT_RGBA8
            : d.format == VT_PIXEL_RGBA16F ? PixelFormat::PIXELFORMAT_RGBA16F : PixelFormat::PIXELFORMAT_RGBA32F;
        options.mipmaps = (d.flags & VT_TEXTURE_MIPMAPS) != 0;
        const bool nearest = (d.flags & VT_TEXTURE_NEAREST) != 0;
        options.magFilter = nearest ? FilterMode::FILTER_NEAREST : FilterMode::FILTER_LINEAR;
        options.minFilter = options.mipmaps
            ? (nearest ? FilterMode::FILTER_NEAREST_MIPMAP_NEAREST : FilterMode::FILTER_LINEAR_MIPMAP_LINEAR)
            : (nearest ? FilterMode::FILTER_NEAREST : FilterMode::FILTER_LINEAR);
        options.name = "vt_texture";
        auto texture = std::make_shared<Texture>(engine->device.get(), options);
        if (d.flags & VT_TEXTURE_CLAMP) {
            texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        }
        if (d.data) {
            texture->setLevelData(0, static_cast<const uint8_t*>(d.data), d.data_size);
            texture->upload();
        }
        void* target = texture.get();
        *out = addResource(engine, vt_engine::ResourceKind::Texture, nullptr, std::move(texture), target);
        return VT_OK;
    });
}

vt_status vt_texture_update(vt_engine* engine, const vt_resource id, const void* data, const size_t dataSize)
{
    return guarded([&]() -> vt_status {
        vt_engine::Resource* resource = nullptr;
        if (const vt_status status = findResource(engine, id, vt_engine::ResourceKind::Texture, "texture", resource);
            status != VT_OK) {
            return status;
        }
        auto* texture = static_cast<Texture*>(resource->target);
        const size_t expected = size_t(texture->width()) * texture->height() * pixelFormatBytesPerPixel(texture->format());
        if (!data || dataSize != expected) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "the pixels must be " + std::to_string(expected) + " bytes");
        }
        texture->setLevelData(0, static_cast<const uint8_t*>(data), dataSize);
        texture->upload();
        return VT_OK;
    });
}

vt_status vt_material_set_texture(vt_engine* engine, const vt_resource materialId, const char* map,
    const vt_resource textureId)
{
    return guarded([&]() -> vt_status {
        vt_engine::Resource* materialResource = nullptr;
        if (const vt_status status = findResource(engine, materialId, vt_engine::ResourceKind::Material, "material",
                materialResource);
            status != VT_OK) {
            return status;
        }
        if (!map) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no map name");
        }
        Texture* texture = nullptr;
        if (textureId != 0) {
            vt_engine::Resource* textureResource = nullptr;
            if (const vt_status status = findResource(engine, textureId, vt_engine::ResourceKind::Texture, "texture",
                    textureResource);
                status != VT_OK) {
                return status;
            }
            texture = static_cast<Texture*>(textureResource->target);
            // The material holds its maps by raw pointer: it keeps the texture alive itself.
            static_cast<Material*>(materialResource->target)->retainResource(textureResource->owner);
        }
        auto* material = static_cast<StandardMaterial*>(materialResource->target);
        const std::string_view name(map);
        if (name == "diffuseMap") {
            material->setDiffuseMap(texture);
        } else if (name == "normalMap") {
            material->setNormalMap(texture);
        } else if (name == "emissiveMap") {
            material->setEmissiveMap(texture);
        } else if (name == "metalnessMap") {
            material->setMetalnessMap(texture);
        } else if (name == "glossMap") {
            material->setGlossMap(texture);
        } else if (name == "opacityMap") {
            material->setOpacityMap(texture);
        } else if (name == "aoMap") {
            material->setAoMap(texture);
        } else {
            return fail(VT_ERROR_INVALID_ARGUMENT, std::string("no map ") + map);
        }
        return VT_OK;
    });
}

vt_status vt_resource_release(vt_engine* engine, const vt_resource id)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (id == engine->sceneResource) {
            return fail(VT_ERROR_INVALID_STATE, "the scene is the engine's and cannot be released");
        }
        if (!(id & VT_RESOURCE_BIT) || engine->resources.erase(id) == 0) {
            return fail(VT_ERROR_NOT_FOUND, "no resource " + std::to_string(id & ~VT_RESOURCE_BIT) + " in this engine");
        }
        return VT_OK;
    });
}

// ---- What an entity draws -------------------------------------------------------------

vt_status vt_render_set_mesh(vt_engine* engine, const vt_entity id, const vt_resource meshId)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        auto* render = entity->findComponent<RenderComponent>();
        if (!render) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no render component");
        }
        vt_engine::Resource* resource = nullptr;
        if (const vt_status status = findResource(engine, meshId, vt_engine::ResourceKind::Mesh, "mesh", resource);
            status != VT_OK) {
            return status;
        }
        std::shared_ptr<Material> material;
        if (const auto it = engine->renderMaterials.find(id); it != engine->renderMaterials.end()) {
            material = it->second;
        } else {
            material = getDefaultMaterial(engine->device);
        }
        // "asset": the component builds no primitive of its own and keeps what it is given.
        render->setType("asset");
        render->clearMeshInstances();
        // The instance co-owns the mesh and the material, so releasing either resource is safe.
        render->addMeshInstance(std::make_unique<MeshInstance>(std::static_pointer_cast<Mesh>(resource->owner),
            material, entity));
        return VT_OK;
    });
}

vt_status vt_render_set_material(vt_engine* engine, const vt_entity id, const vt_resource materialId)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        auto* render = entity->findComponent<RenderComponent>();
        if (!render) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no render component");
        }
        std::shared_ptr<Material> material;
        if (materialId != 0) {
            vt_engine::Resource* resource = nullptr;
            if (const vt_status status = findResource(engine, materialId, vt_engine::ResourceKind::Material,
                    "material", resource);
                status != VT_OK) {
                return status;
            }
            material = std::static_pointer_cast<Material>(resource->owner);
            engine->renderMaterials[id] = material;
        } else {
            material = getDefaultMaterial(engine->device);
            engine->renderMaterials.erase(id);
        }
        render->setMaterial(material.get());
        for (MeshInstance* meshInstance : render->meshInstances()) {
            if (meshInstance) {
                meshInstance->setMaterial(material);
            }
        }
        return VT_OK;
    });
}

// ---- Offscreen rendering and capture ----------------------------------------------------

vt_status vt_camera_set_offscreen(vt_engine* engine, const vt_entity id, const uint32_t width, const uint32_t height)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        auto* camera = entity->findComponent<CameraComponent>();
        if (!camera) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(id) + " has no camera");
        }
        if (width == 0 || height == 0) {
            camera->camera()->setRenderTarget(nullptr);
            engine->offscreen.erase(id);
            return VT_OK;
        }
        const auto maxSize = static_cast<uint32_t>(engine->device->maxTextureSize());
        if (width > maxSize || height > maxSize) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "an offscreen image is at most " + std::to_string(maxSize) +
                " pixels a side");
        }
        TextureOptions textureOptions;
        textureOptions.width = width;
        textureOptions.height = height;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        textureOptions.name = "vt_offscreen";
        vt_engine::Offscreen offscreen;
        offscreen.texture = std::make_shared<Texture>(engine->device.get(), textureOptions);
        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = engine->device.get();
        targetOptions.name = "vt_offscreen";
        targetOptions.colorBuffer = offscreen.texture.get();
        targetOptions.depth = true;
        // Antialiased as the window's back buffer is.
        targetOptions.samples = std::max(1, engine->device->samples());
        offscreen.target = engine->device->createRenderTarget(targetOptions);
        if (!offscreen.target) {
            return fail(VT_ERROR_UNSUPPORTED, "this backend cannot render offscreen");
        }
        camera->camera()->setRenderTarget(offscreen.target);
        engine->offscreen[id] = std::move(offscreen);
        return VT_OK;
    });
}

vt_status vt_camera_read_pixels(vt_engine* engine, const vt_entity id, uint8_t* buffer, const size_t capacity,
    uint32_t* width, uint32_t* height)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        const auto it = engine->offscreen.find(id);
        if (it == engine->offscreen.end()) {
            return fail(VT_ERROR_INVALID_STATE, "the camera does not render offscreen (vt_camera_set_offscreen)");
        }
        const Texture& texture = *it->second.texture;
        if (width) {
            *width = texture.width();
        }
        if (height) {
            *height = texture.height();
        }
        const size_t bytes = size_t(texture.width()) * texture.height() * 4;
        if (!buffer) {
            return VT_OK;
        }
        if (capacity < bytes) {
            return fail(VT_ERROR_BUFFER_TOO_SMALL, "the pixels need " + std::to_string(bytes) + " bytes");
        }
        if (engine->backend == VT_BACKEND_NULL) {
            return fail(VT_ERROR_UNSUPPORTED, "the null backend has no pixels to read");
        }
        std::vector<uint8_t> pixels;
        if (!it->second.texture->read(pixels) || pixels.size() < bytes) {
            return fail(VT_ERROR_INTERNAL, "the offscreen image could not be read back");
        }
        std::memcpy(buffer, pixels.data(), bytes);
        return VT_OK;
    });
}

// ---- Models and loaded textures ----------------------------------------------------------

vt_status vt_gltf_load(vt_engine* engine, const char* path, const vt_entity parent, vt_entity* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!path || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no path or no output");
        }
        Entity* parentEntity = engine->engine->root();
        if (parent != 0) {
            if (const vt_status status = findEntity(engine, parent, parentEntity); status != VT_OK) {
                return status;
            }
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return fail(VT_ERROR_NOT_FOUND, std::string("no file ") + path);
        }
        // The asset only builds the model: the entities co-own their meshes and materials, and the
        // materials keep the container's textures, so the asset goes when the call returns.
        Asset asset(std::filesystem::path(path).filename().string(), AssetType::CONTAINER, path);
        auto* container = asset.resourceAs<ContainerResource>();
        if (!container) {
            return fail(VT_ERROR_INVALID_ARGUMENT, std::string("not a glTF model the engine can read: ") + path);
        }
        Entity* model = container->instantiateRenderEntity();
        if (!model) {
            return fail(VT_ERROR_INTERNAL, std::string("the model could not be built: ") + path);
        }
        model->setEngine(engine->engine.get());
        parentEntity->addChild(model);
        *out = model->id().value;
        return VT_OK;
    });
}

vt_status vt_texture_load(vt_engine* engine, const char* path, const uint32_t flags, vt_resource* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!path || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no path or no output");
        }
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            return fail(VT_ERROR_NOT_FOUND, std::string("no file ") + path);
        }
        AssetData data;
        data.mipmaps = (flags & VT_TEXTURE_MIPMAPS) != 0;
        if (flags & VT_TEXTURE_ENV_ATLAS) {
            // Its rects are its levels: no mips, decoded as RGBP.
            data.type = TextureType::TEXTURETYPE_RGBP;
            data.mipmaps = false;
        }
        // The asset owns the texture, so the resource keeps the asset.
        auto asset = std::make_shared<Asset>(std::filesystem::path(path).filename().string(), AssetType::TEXTURE,
            path, data);
        auto* texture = asset->resourceAs<Texture>();
        if (!texture) {
            return fail(VT_ERROR_INVALID_ARGUMENT, std::string("not an image the engine can read: ") + path);
        }
        if (flags & VT_TEXTURE_NEAREST) {
            texture->setMagFilter(FilterMode::FILTER_NEAREST);
            texture->setMinFilter(data.mipmaps ? FilterMode::FILTER_NEAREST_MIPMAP_NEAREST : FilterMode::FILTER_NEAREST);
        }
        if (flags & VT_TEXTURE_CLAMP) {
            texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
            texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        }
        *out = addResource(engine, vt_engine::ResourceKind::Texture, nullptr, std::move(asset), texture);
        return VT_OK;
    });
}

vt_status vt_scene_set_environment(vt_engine* engine, const vt_resource id)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (id == 0) {
            engine->engine->scene()->setEnvAtlas(nullptr);
            engine->environment.reset();
            return VT_OK;
        }
        vt_engine::Resource* resource = nullptr;
        if (const vt_status status = findResource(engine, id, vt_engine::ResourceKind::Texture, "texture", resource);
            status != VT_OK) {
            return status;
        }
        engine->engine->scene()->setEnvAtlas(static_cast<Texture*>(resource->target));
        engine->environment = resource->owner;
        return VT_OK;
    });
}

vt_status vt_entity_bounds(vt_engine* engine, const vt_entity id, vt_vec3* center, vt_vec3* halfExtents)
{
    return guarded([&]() -> vt_status {
        Entity* entity = nullptr;
        if (const vt_status status = findEntity(engine, id, entity); status != VT_OK) {
            return status;
        }
        if (!center || !halfExtents) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        BoundingBox bounds;
        bool any = false;
        for (auto* render : entity->findComponents<RenderComponent>()) {
            if (!render) {
                continue;
            }
            for (MeshInstance* meshInstance : render->meshInstances()) {
                if (!meshInstance) {
                    continue;
                }
                // The first box seeds the union: added to an empty box at the origin, every
                // bounds would reach the origin.
                if (any) {
                    bounds.add(meshInstance->aabb());
                } else {
                    bounds = meshInstance->aabb();
                }
                any = true;
            }
        }
        if (!any) {
            bounds.setCenter(entity->position());
            bounds.setHalfExtents(0.5f, 0.5f, 0.5f);
        }
        const Vector3 c = bounds.center();
        const Vector3 h = bounds.halfExtents();
        *center = vt_vec3{c.getX(), c.getY(), c.getZ()};
        *halfExtents = vt_vec3{h.getX(), h.getY(), h.getZ()};
        return VT_OK;
    });
}

// ---- Input and picking ----------------------------------------------------------------

vt_status vt_input_get(vt_engine* engine, vt_input_state* out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        return fillSized(out, engine->input);
    });
}

vt_status vt_key_get(vt_engine* engine, const int32_t scancode, int32_t* down, int32_t* pressed)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (scancode < 0 || scancode >= SDL_SCANCODE_COUNT) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no scancode " + std::to_string(scancode));
        }
        if (down) {
            *down = engine->keysDown[static_cast<size_t>(scancode)];
        }
        if (pressed) {
            *pressed = engine->keysPressed[static_cast<size_t>(scancode)];
        }
        return VT_OK;
    });
}

vt_status vt_pick(vt_engine* engine, const vt_entity cameraId, const float x, const float y, int32_t* hit,
    vt_entity* out, vt_vec3* point)
{
    return guarded([&]() -> vt_status {
        Entity* cameraEntity = nullptr;
        if (const vt_status status = findEntity(engine, cameraId, cameraEntity); status != VT_OK) {
            return status;
        }
        auto* camera = cameraEntity->findComponent<CameraComponent>();
        if (!camera) {
            return fail(VT_ERROR_NO_TARGET, "entity " + std::to_string(cameraId) + " has no camera");
        }
        if (!hit || !out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *hit = 0;
        *out = 0;
        // The id buffer at the window's size in points, so a point of the window is its pixel.
        const auto [width, height] = engine->engine->canvasSize();
        const int w = std::max(1, width);
        const int h = std::max(1, height);
        if (!engine->picker) {
            engine->picker = std::make_unique<Picker>(engine->engine.get(), w, h, true);
        } else if (engine->picker->width() != w || engine->picker->height() != h) {
            engine->picker->resize(w, h);
        }
        engine->picker->prepare(camera, engine->engine->scene().get());
        const int px = static_cast<int>(std::floor(x));
        const int py = static_cast<int>(std::floor(y));
        if (px < 0 || py < 0 || px >= w || py >= h) {
            return VT_OK;
        }
        MeshInstance* meshInstance = engine->picker->getSelectionSingle(px, py);
        if (!meshInstance) {
            return VT_OK;
        }
        // The entity whose render component drew it: the instance's node or the nearest entity above.
        for (GraphNode* node = meshInstance->node(); node; node = node->parent()) {
            if (auto* entity = dynamic_cast<Entity*>(node)) {
                *out = entity->id().value;
                *hit = 1;
                break;
            }
        }
        if (point && *hit) {
            if (const auto world = engine->picker->getWorldPoint(px, py)) {
                *point = vt_vec3{world->getX(), world->getY(), world->getZ()};
            } else {
                *point = vt_vec3{0.0f, 0.0f, 0.0f};
            }
        }
        return VT_OK;
    });
}

} // extern "C"

// ---- Extensions -------------------------------------------------------------------------

vt_status vt_engine_native(vt_engine* engine, void** out)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!out) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no output");
        }
        *out = engine->engine.get();
        return VT_OK;
    });
}

vt_status vt_set_error(const vt_status status, const char* message)
{
    if (status == VT_OK) {
        lastError.clear();
        return VT_OK;
    }
    return fail(status, message ? message : "");
}

vt_status vt_engine_on_destroy(vt_engine* engine, const vt_engine_destroy_callback callback, void* user)
{
    return guarded([&]() -> vt_status {
        if (const vt_status status = checkEngine(engine); status != VT_OK) {
            return status;
        }
        if (!callback) {
            return fail(VT_ERROR_INVALID_ARGUMENT, "no callback");
        }
        engine->onDestroy.emplace_back(callback, user);
        return VT_OK;
    });
}
