// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Common host for the example applications.
//
// Every example used to open with the same ~60 lines: SDL init, window, renderer,
// swap chain, graphics device, AppOptions with the same four component systems,
// Engine init/start, a performance-counter delta-time loop, and a teardown lambda.
// ExampleApp owns all of that so an example file contains only the scene it exists
// to demonstrate.
//
// Deriving from it looks like this:
//
//     class MyExample final: public ExampleApp
//     {
//     public:
//         MyExample(): ExampleApp({.title = "My Example"}) {}
//
//     protected:
//         bool create() override { ...build the scene...; return true; }
//         void update(float dt) override { ...per-frame...; }
//     };
//
//     VISUTWIN_EXAMPLE_MAIN(MyExample)
//
// Backend portability is the other reason this exists. The Metal path needs
// metal-cpp's *_PRIVATE_IMPLEMENTATION translation unit and a CAMetalLayer from
// an SDL renderer; the Vulkan path needs SDL_WINDOW_VULKAN at window-creation
// time. Both now live in exampleApp.cpp, so an example compiles for either
// backend without carrying a single #ifdef of its own.
//
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include "core/math/color.h"
#include "core/shape/boundingBox.h"
#include "core/math/vector3.h"
#include "framework/appOptions.h"
#include "framework/engine.h"
#include "framework/entity.h"
// The four component systems ExampleApp registers, plus their components.
// Entity::addComponent<T>() instantiates against the system type, so an example
// calling it needs the system header in scope — carrying them here keeps that
// dependency out of 41 files.
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/script/scriptComponent.h"
#include "framework/components/script/scriptComponentSystem.h"
// Every example logs, so the spdlog wrapper comes along with the host rather
// than being repeated in 41 files.
#include "log.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class CameraControls;

    /// Window and host configuration, passed up from the derived example's
    /// constructor. Designated initialisers keep the call site readable:
    /// `ExampleApp({.title = "Clear Coat", .width = 1280})`.
    struct ExampleOptions
    {
        std::string title = "VisuTwin Canvas";

        int width = 900;
        int height = 700;

        /// Examples log at debug level by default — they are development tools,
        /// and the shader/asset chatter is usually the point.
        bool debugLogging = true;
    };

    /**
     * @brief Base class for the example applications: window, graphics device,
     *        engine, and frame loop.
     *
     * run() drives the whole lifecycle and calls the virtual hooks in order:
     * configure() → create() → [update() → preRender() → postRender()]* →
     * destroy(). Only create() is mandatory.
     */
    class ExampleApp
    {
    public:
        virtual ~ExampleApp();

        ExampleApp(const ExampleApp&) = delete;
        ExampleApp& operator=(const ExampleApp&) = delete;

        /// Runs the example to completion. Returns 0 on a clean exit, -1 when
        /// setup failed (the reason is logged).
        int run();

    protected:
        explicit ExampleApp(ExampleOptions options);

        // ── Hooks ────────────────────────────────────────────────────────────

        /// Register component systems beyond the four every example gets
        /// (Render, Camera, Light, Script). Called before Engine::init.
        virtual void configure(AppOptions& options) { (void)options; }

        /// Build the scene. Return false to abort startup — log the reason
        /// first, run() only reports that create() failed.
        virtual bool create() = 0;

        /// Per-frame work, called before Engine::update.
        virtual void update(float dt) { (void)dt; }

        /// Per-frame work that must sit BETWEEN Engine::update and
        /// Engine::render — the element system's glyph-mesh sync and a compute
        /// dispatch that has to be encoded ahead of the frame both need this
        /// slot, not update().
        virtual void preRender() {}

        /// Per-frame work that has to observe the rendered frame, called after
        /// Engine::render — a dynamic ReflectionProbe's mip regeneration and the
        /// GPU lightmapper's readback both belong here.
        virtual void postRender() {}

        /// Handle an SDL event. Return true to consume it; anything left
        /// unconsumed falls through to the default bindings (Esc/quit, and the
        /// zoom and reset input of a camera created by addOrbitControls).
        virtual bool onEvent(const SDL_Event& event) { (void)event; return false; }

        /// Release anything that borrows the engine or the graphics device.
        /// Called while both are still alive, unlike a derived destructor — and
        /// also when create() fails, so it must tolerate a partially built scene.
        virtual void destroy() {}

        // ── Services ─────────────────────────────────────────────────────────

        [[nodiscard]] Engine* engine() const { return _engine.get(); }

        /// The engine's owning pointer, for the few APIs that take a
        /// shared_ptr<Engine> (MiniStats, the lightmappers).
        [[nodiscard]] const std::shared_ptr<Engine>& enginePtr() const { return _engine; }
        [[nodiscard]] const std::shared_ptr<Scene>& scene() const { return _engine->scene(); }
        [[nodiscard]] Entity* root() const { return _engine->root(); }
        [[nodiscard]] const std::shared_ptr<GraphicsDevice>& device() const { return _device; }
        [[nodiscard]] SDL_Window* window() const { return _window; }

        /// The backend actually in use — Vulkan when compiled in, else Metal,
        /// with VISUTWIN_BACKEND overriding either.
        [[nodiscard]] Backend backend() const { return _backend; }

        /// Seconds since the first frame — saves an example keeping its own
        /// accumulator when an animation phase can read straight off it.
        [[nodiscard]] float elapsedTime() const { return _elapsed; }

        /// Ends the frame loop after the current frame.
        void quit() { _running = false; }

        /// The window size the example asked for, in points. A screen-space effect
        /// that has to size itself needs this before the first frame.
        [[nodiscard]] int windowWidth() const { return _options.width; }
        [[nodiscard]] int windowHeight() const { return _options.height; }

        /// Absolute path to a shared asset, e.g. assetPath("models/fox.glb").
        [[nodiscard]] static std::string assetPath(std::string_view relative);

        // ── Scene helpers ────────────────────────────────────────────────────
        // The pieces of scene setup that repeat across most examples. An example
        // wanting anything more specific builds the entity itself — these exist
        // to remove noise, not to hide the API.

        /// A camera entity parented to the root, at `position`, looking along
        /// `eulerAngles` (degrees, pitch/yaw/roll).
        Entity* createCamera(const Vector3& position, const Vector3& eulerAngles = Vector3(0.0f, 0.0f, 0.0f));

        /// Attaches the CameraControls script to `camera` in orbit mode around
        /// `focusPoint`, and routes wheel/pinch zoom and the R reset key to it.
        /// The returned script stays owned by the entity.
        CameraControls* addOrbitControls(Entity* camera, const Vector3& focusPoint = Vector3(0.0f, 0.0f, 0.0f));

        /// A procedural-primitive entity ("box", "sphere", "plane", "cone", ...)
        /// with `material`, parented to the root. Pass `layers` to place it
        /// somewhere other than the render component's default layer.
        Entity* createPrimitive(const char* type, Material* material,
                                const Vector3& position = Vector3(0.0f, 0.0f, 0.0f),
                                const Vector3& scale = Vector3(1.0f, 1.0f, 1.0f),
                                const std::vector<int>& layers = {});

        /// World-space AABB over every mesh instance owned by `entity` or any of
        /// its descendants — what several examples use to auto-frame a loaded
        /// model. Falls back to a half-unit box at the entity's position when it
        /// has no mesh instances at all.
        static BoundingBox entityBounds(Entity* entity);

        /// A directional light entity parented to the root, aimed by
        /// `eulerAngles` (degrees). castShadows defaults to false to match
        /// LightComponent's own default — a helper that quietly turned shadows
        /// on would change how every scene using it renders.
        Entity* createDirectionalLight(const Vector3& eulerAngles,
                                       const Color& color = Color(1.0f, 1.0f, 1.0f, 1.0f),
                                       float intensity = 1.0f,
                                       bool castShadows = false);

    private:
        bool initWindow();
        bool initEngine();
        void handleEvent(const SDL_Event& event);
        void shutdown();

        ExampleOptions _options;

        SDL_Window* _window = nullptr;
        SDL_Renderer* _renderer = nullptr;
        Backend _backend = Backend::Metal;

        std::shared_ptr<GraphicsDevice> _device;
        std::shared_ptr<Engine> _engine;

        CameraControls* _cameraControls = nullptr;

        bool _running = true;
        float _elapsed = 0.0f;
    };
}

/// Generates the example's main(). Keep it at file scope, after the class.
#define VISUTWIN_EXAMPLE_MAIN(ExampleClass)      \
    int main(int, char**)                        \
    {                                            \
        ExampleClass app;                        \
        return app.run();                        \
    }
