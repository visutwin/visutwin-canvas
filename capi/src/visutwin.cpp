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
// This file holds the version, the errors, the engine and its frame, and the extension entry
// points; the other sections of visutwin.h are in capi*.cpp, the shared helpers in
// capiSupport.cpp (capiInternal.h).
#include "capiInternal.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::capi;

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
        {
            vt_engine* owner = engine.get();
            const auto systems = engine->engine->systems();
            engine->renderRemoved = systems->getById("render")->on("beforeremove",
                [owner](Entity* entity, Component*) { owner->renderMaterials.erase(entity->id().value); });
            engine->cameraRemoved = systems->getById("camera")->on("beforeremove",
                [owner](Entity* entity, Component*) { owner->offscreen.erase(entity->id().value); });
        }
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
    return guarded([&] { return fail(status, message ? message : ""); });
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
