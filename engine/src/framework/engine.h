// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025.
//
#pragma once

#include <SDL3/SDL.h>

#include "appOptions.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/scene.h"
#include "scene/renderer/forwardRenderer.h"
#include "applicationStats.h"
#include "constants.h"
#include "sceneRegistry.h"
#include "assets/assetRegistry.h"
#include "bundles/bundleRegistry.h"
#include "handlers/resourceLoader.h"
#include "i18n/i18n.h"
#include "platform/input/controller.h"
#include "platform/input/mouse.h"
#include "script/scriptRegistry.h"

namespace visutwin::canvas
{
    class ComponentSystemRegistry;

    /**
     * @brief Central application orchestrator managing scenes, rendering, input, and resource loading.
     * @ingroup group_framework_ecs
     *
     * Engine is the entry point for a VisuTwin Canvas application. It owns the GraphicsDevice,
     * Scene, ForwardRenderer, component systems, and the async ResourceLoader. A typical frame
     * calls update() -> fixedUpdate() (zero or more substeps) -> render().
     */
    class Engine : public EventHandler, public std::enable_shared_from_this<Engine>
    {
    public:
        Engine(SDL_Window* window) : _window(window) {}

        virtual ~Engine();

        void init(const AppOptions& appOptions);

        // Destroys application and removes all event listeners
        void destroy();

        // Start the application
        void start();

        const std::shared_ptr<Scene>& scene() const { return _scene; }
        const std::shared_ptr<GraphicsDevice>& graphicsDevice() const { return _graphicsDevice; }

        // Controls how the canvas fills the window and resizes when the window changes
        void setCanvasFillMode(FillMode mode, int width = 0, int height = 0);

        // Change the resolution of the canvas
        void setCanvasResolution(ResolutionMode mode, int width = 0, int height = 0);

        // Resize the application's canvas
        std::pair<int, int> resizeCanvas(int width = 0, int height = 0);

        // Updates the {@link GraphicsDevice} canvas size to match the canvas size on the document page.
        // It is recommended to call this function when the canvas size changes
        // (e.g. on window resize and orientation change events) so that the canvas resolution is immediately updated.
        void updateCanvasSize();

        // Update the engine (variable timestep — called once per frame)
        void update(float dt);

        // Run one fixed-timestep simulation substep (deterministic dt)
        void fixedUpdate(float fixedDt);

        // Update all input devices managed by the engine
        void inputUpdate(float dt);

        // Render the application's scene for one frame
        void render();

        // Fixed timestep configuration
        float fixedDeltaTime() const { return _fixedDeltaTime; }
        void setFixedDeltaTime(float dt) { _fixedDeltaTime = dt; }

        int maxFixedSubSteps() const { return _maxFixedSubSteps; }
        void setMaxFixedSubSteps(int n) { _maxFixedSubSteps = n; }

        // Interpolation alpha (0..1) between the last two fixed-update states.
        // Use in rendering to smooth visual positions when sim rate != frame rate.
        float fixedTimeAlpha() const { return _fixedTimeAlpha; }

        std::shared_ptr<Entity> root() { return _root; }

        std::shared_ptr<ScriptRegistry> scripts() const { return _scripts; }

        std::shared_ptr<ComponentSystemRegistry> systems() const { return _systems; }

        SDL_Window* sdlWindow() const { return _window; }

        /**batcher accessor. */
        BatchManager* batcher() { return _batcher.get(); }

        /** Async resource loader — single background I/O thread with main-thread callbacks. */
        const std::shared_ptr<ResourceLoader>& loader() const { return _loader; }

    protected:
        virtual double processTimestamp(double timestamp) { return timestamp; }

    private:
        friend MakeTickCallback makeTick(const std::shared_ptr<Engine>& engine);

        static std::unordered_map<std::string, std::shared_ptr<Engine>> _engines;

        // Render a layer composition
        void renderComposition();

        void tick();

        void initDefaultMaterial();

        void initProgramLibrary();

        void registerSceneImmediate(const std::shared_ptr<Scene>& scene);

        void fillFrameStatsBasic(double now, float dt, float ms);

        void fillFrameStats();

        std::shared_ptr<GraphicsDevice> _graphicsDevice;

        std::shared_ptr<Entity> _root;
        std::shared_ptr<ForwardRenderer> _renderer;

        std::shared_ptr<Scene> _scene;

        // The application's component system registry
        std::shared_ptr<ComponentSystemRegistry> _systems;

        void* _frameRequestId = nullptr;
        float _timeScale = 1.0f;
        float _maxDeltaTime = 0.1f;

        // The total number of frames the application has updated since start() was called
        long _frame = 0;

        // When true, the application's render function is called every frame
        bool _autoRender = true;

        std::shared_ptr<ApplicationStats> _stats;

        std::shared_ptr<ResourceLoader> _loader;
        std::shared_ptr<AssetRegistry> _assets;
        std::shared_ptr<BundleRegistry> _bundles;
        std::shared_ptr<SceneRegistry> _scenes;
        std::shared_ptr<ScriptRegistry> _scripts;
        std::shared_ptr<I18n> _i18n;

        // Default layers
        std::shared_ptr<Layer> _defaultLayerWorld;
        std::shared_ptr<Layer> _defaultLayerDepth;
        std::shared_ptr<Layer> _defaultLayerSkybox;
        std::shared_ptr<Layer> _defaultLayerUi;
        std::shared_ptr<Layer> _defaultLayerImmediate;

        std::vector<std::string> _scriptsOrder;
        std::string _scriptPrefix;

        std::shared_ptr<Lightmapper> _lightmapper;

        std::shared_ptr<BatchManager> _batcher;

        std::shared_ptr<Controller> _controller;
        std::shared_ptr<Keyboard> _keyboard;
        std::shared_ptr<Mouse> _mouse;
        std::shared_ptr<GamePads> _gamepads;
        std::shared_ptr<TouchDevice> _touch;
        std::shared_ptr<ElementInput> _elementInput;
        std::shared_ptr<XrManager> _xr;

        MakeTickCallback _tick;

        FillMode _fillMode = FillMode::FILLMODE_KEEP_ASPECT;

        ResolutionMode _resolutionMode = ResolutionMode::RESOLUTION_FIXED;

        bool _allowResize = true;

        bool _inFrameUpdate = false;

        double _time = 0.0;

        // Fixed timestep state
        float _fixedDeltaTime = 1.0f / 60.0f;
        double _fixedTimeAccumulator = 0.0;
        int _maxFixedSubSteps = 8;
        float _fixedTimeAlpha = 0.0f;

        bool _renderNextFrame = false;

        bool _destroyRequested = false;

        std::unordered_map<std::string, std::shared_ptr<Entity>> _entityIndex;

        bool _inTools = false;

        // Runtime parity checks for frame lifecycle ordering.
        bool _frameStartCalled = false;
        bool _renderCompositionCalled = false;
        bool _frameEndCalled = false;

        SDL_Window* _window;
    };

    MakeTickCallback makeTick(const std::shared_ptr<Engine>& engine);
}
