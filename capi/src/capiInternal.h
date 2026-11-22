// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.11.2026
//
// Internal to the C API: the engine record behind a vt_engine and the helpers every entry
// point uses (the thread's error, the exception guard, the engine and entity checks, the
// typed property access). The entry points themselves are in visutwin.cpp and capi*.cpp, one
// file per section of visutwin.h. Not installed; include it from those files only.
#pragma once

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


struct vt_engine
{
    std::thread::id thread;
    vt_backend backend = VT_BACKEND_NULL;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool sdlInitialized = false;
    bool gamepadInitialized = false;
    std::shared_ptr<visutwin::canvas::GraphicsDevice> device;
    std::shared_ptr<visutwin::canvas::ElementInput> elementInput;
    std::shared_ptr<visutwin::canvas::Engine> engine;
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
        std::shared_ptr<visutwin::canvas::Texture> depth;   // sampleable: the scene depth map reads it
        std::shared_ptr<visutwin::canvas::RenderTarget> target;
    };
    std::unordered_map<uint64_t, Offscreen> offscreen;

    // renderMaterials and offscreen belong to one COMPONENT, not to the entity: these clear an
    // entry when its component goes, by whatever path (vt_component_remove, the entity
    // destroyed, a script), so a component added again starts without its predecessor's state.
    visutwin::canvas::EventHandlePtr renderRemoved;
    visutwin::canvas::EventHandlePtr cameraRemoved;

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

namespace visutwin::canvas::capi
{
    // The thread's last error, one for the whole library (defined in capiSupport.cpp): every
    // file reads and writes the same one. Not an inline variable: ld64 rejects the per-file
    // initialisation routines an inline thread_local std::string emits as duplicates.
    extern thread_local std::string lastError;

    vt_status fail(const vt_status status, std::string message);

    vt_status failInternal(const char* what) noexcept;


    // Runs one entry point: a clear error to start, every exception caught.
    template <class Body>
    vt_status guarded(Body&& body)
    {
        lastError.clear();
        try {
            return body();
        } catch (const std::exception& exception) {
            return failInternal(exception.what());
        } catch (...) {
            return failInternal(nullptr);
        }
    }

    vt_status checkEngine(const vt_engine* engine);

    vt_status findEntity(vt_engine* engine, const vt_entity id, Entity*& out);

    vt_status writeString(const std::string_view text, char* buffer, const size_t capacity, size_t* length);

    vt_status fromPropertyStatus(const PropertyStatus status, const std::string_view what);

    const TypeDesc* typeOf(const vt_type type);

    vt_type handleOf(const TypeDesc* type);

    const PropertyDesc* propertyOf(const vt_property property, const TypeDesc** owner = nullptr);

    vt_value_type valueTypeOf(const ValueType type);


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

    std::string qualifiedName(const TypeDesc& type, const PropertyDesc& property);

    vt_status resolveProperty(vt_engine* engine, const vt_entity id, const vt_property handle,
        const PropertyDesc*& property, void*& target, const TypeDesc** ownerOut = nullptr);

    bool typeAccepts(const ValueType actual, const ValueType asked);


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
        const PropertyValue& value);

    const TypeDesc* componentType(const char* name, vt_status& status);

    void countFrame(vt_engine* engine);

    vt_resource addResource(vt_engine* engine, vt_engine::ResourceKind kind, const char* typeName,
        std::shared_ptr<void> owner, void* target);

    vt_status findResource(vt_engine* engine, const vt_resource id, const vt_engine::ResourceKind kind,
        const char* kindName, vt_engine::Resource*& out);

    uint32_t bytesPerPixel(const vt_pixel_format format);

    vt_status vertexGeometry(const vt_mesh_desc& d, const std::vector<uint32_t>& indices, PrimitiveGeometry& geometry);

    void startIfNeeded(vt_engine* engine);

    void teardown(vt_engine* engine);
}
