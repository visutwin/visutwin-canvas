// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025
//
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <SDL3/SDL.h>

#include "appOptions.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/scene.h"
#include "scene/renderer/forwardRenderer.h"
#include "scene/shader-lib/shaderSourceWatcher.h"
#include "applicationStats.h"
#include "constants.h"
#include "entityId.h"
#include "sceneRegistry.h"
#include "assets/assetRegistry.h"
#include "bundles/bundleRegistry.h"
#include "handlers/resourceLoader.h"
#include "i18n/i18n.h"
#include "platform/input/controller.h"
#include "platform/input/gamePads.h"
#include "platform/input/keyboard.h"
#include "platform/input/mouse.h"
#include "platform/input/touchDevice.h"
#include "script/scriptRegistry.h"

namespace visutwin::canvas
{
    class PhysicsWorld;
    class ComponentSystemRegistry;
    class ComponentRegistry;

    /**
     * @brief Central application orchestrator managing scenes, rendering, input, and resource loading.
     * @ingroup group_framework_ecs
     *
     * Engine is the entry point for a VisuTwin Canvas application. It owns the GraphicsDevice,
     * Scene, ForwardRenderer, component systems, and the async ResourceLoader. A typical frame
     * calls update() -> fixedUpdate() (zero or more substeps) -> render().
     */
    /// The options of Engine::precompile.
    struct PrecompileOptions
    {
        /// Build only for the mesh instances the cameras see, not for everything in their
        /// layers: fewer pipelines up front, the rest built when they come into view.
        bool frustumCulling = false;
    };

    /// What an Engine::precompile did.
    struct PrecompileStats
    {
        int programs = 0;       ///< Slang programs it compiled on compile threads (chunk overrides)
        int shaders = 0;        ///< shaders it created
        int pipelines = 0;      ///< render pipelines it asked to build
        double frameTime = 0.0; ///< milliseconds of the frame that started it
        double time = 0.0;      ///< milliseconds until every pipeline had built
    };

    class Engine : public EventHandler, public std::enable_shared_from_this<Engine>
    {
    public:
        explicit Engine(SDL_Window* window);

        virtual ~Engine();

        void init(const AppOptions& appOptions);

        // Destroys application and removes all event listeners
        void destroy();

        // Start the application
        void start();

        const std::shared_ptr<Scene>& scene() const { return _scene; }

        /// This engine's live components, a creation-ordered list per type
        /// (`components().instances<RenderComponent>()`). Another engine in the process
        /// has its own.
        ComponentRegistry& components() const { return *_components; }
        const std::shared_ptr<GraphicsDevice>& graphicsDevice() const { return _graphicsDevice; }
        const std::shared_ptr<ForwardRenderer>& renderer() const { return _renderer; }

        /// The XR manager the application supplied (AppOptions::xr), or null.
        const std::shared_ptr<XrManager>& xr() const { return _xr; }

        /// Per-frame CPU timings, draw-call and primitive counts. Populated every frame; read by
        /// the MiniStats HUD.
        const std::shared_ptr<ApplicationStats>& stats() const { return _stats; }

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

        /**
         * Update the engine (variable timestep — called once per frame). `dt` is the
         * measured frame time in seconds; it is clamped to [0, maxDeltaTime()] and then
         * multiplied by timeScale() before anything sees it, so every system, script,
         * the "update" event and the fixed-update accumulator receive the same step.
         */
        void update(float dt);

        /// Multiplier applied to every frame's time step: 0.5 runs the world at half speed,
        /// 0 freezes everything that advances with time. It does not pause scripts: their
        /// update and postUpdate still run every frame with a dt of 0, so input handling,
        /// spawning, impulses and any other logic not multiplied by dt carry on; give
        /// gameplay code its own pause check. To pause physics alone, use
        /// RigidBodyComponentSystem::setTimeScale.
        float timeScale() const { return _timeScale; }
        void setTimeScale(float scale) { _timeScale = scale; }

        /// The longest frame time update() passes on, in seconds (0.1 by default). A frame
        /// that took longer (a stall, a breakpoint, a window drag) advances the world by this
        /// much instead of jumping.
        float maxDeltaTime() const { return _maxDeltaTime; }
        void setMaxDeltaTime(float seconds) { _maxDeltaTime = seconds; }

        /// The time every update() so far has advanced the world by, in seconds: the sum of
        /// the steps after the clamp and the time scale. Anything that advances with the world
        /// but runs elsewhere (a camera frame's effects, which update as it renders) takes its
        /// own step as the difference since it last looked, so it pauses with the world and a
        /// frame rendered again without an update advances it by nothing.
        double updatedTime() const { return _updatedTime; }

        // Run one fixed-timestep simulation substep (deterministic dt)
        void fixedUpdate(float fixedDt);

        // Update all input devices managed by the engine
        void inputUpdate(float dt);

        // Render the application's scene for one frame
        void render();

        /**
         * Builds the shaders and render pipelines the next frame needs without rendering it, so
         * they compile in parallel on compile threads instead of one by one in the frame's draws
         * (a cold first frame of a few seconds becomes the time of the slowest few). The next
         * render() builds and culls the frame as usual, runs its passes' draws into precompile
         * passes (GraphicsDevice::beginPrecompilePass), and renders nothing; the renders after
         * it render nothing either, the display keeping what it showed, until every pipeline
         * has built, and the render after that renders the frame. The update keeps running.
         * The mesh instances in the cameras' layers take part, off screen too, unless
         * `frustumCulling`; anything else builds when it first renders, as before.
         *
         * Call it after adding content that needs new shaders: before the first frame, or
         * after loading a level. The engine fires "precompile:start" once the builds started,
         * "precompile:progress" (float, the fraction built, 0 to 1) in the frames it changed,
         * and "precompile:end" (PrecompileStats) right before the frame that renders with
         * everything built; `onDone` is called with the same statistics. A call while a
         * precompile runs adds its callback to that one. If the engine is destroyed first,
         * the callbacks run then, with what was counted so far.
         */
        void precompile(const PrecompileOptions& options = {},
            std::function<void(const PrecompileStats&)> onDone = {});
        [[nodiscard]] bool precompiling() const { return _precompile != nullptr; }

        /// Block until the next frame can begin (GraphicsDevice::waitForNextFrame). A loop
        /// calls it at the top, before polling input, so the frame shows the freshest input
        /// rather than input one display interval old.
        void waitForNextFrame();

        // Fixed timestep configuration
        float fixedDeltaTime() const { return _fixedDeltaTime; }
        void setFixedDeltaTime(float dt) { _fixedDeltaTime = dt; }

        int maxFixedSubSteps() const { return _maxFixedSubSteps; }
        void setMaxFixedSubSteps(int n) { _maxFixedSubSteps = n; }

        // Interpolation alpha (0..1) between the last two fixed-update states.
        // Use in rendering to smooth visual positions when sim rate != frame rate.
        float fixedTimeAlpha() const { return _fixedTimeAlpha; }

        Entity* root() const { return _root.get(); }

        /// The live entity of this engine's hierarchy with this id, or null (Entity::findById
        /// keeps to no engine).
        Entity* entityById(EntityId id) const;

        /// The entity at a path under the root (Entity::path: names joined by '/', the first
        /// child of each name where siblings share one; empty is the root), or null.
        Entity* entityByPath(std::string_view path) const;

        std::shared_ptr<ScriptRegistry> scripts() const { return _scripts; }

        std::shared_ptr<ComponentSystemRegistry> systems() const { return _systems; }

        SDL_Window* sdlWindow() const { return _window; }

        /**
         * The canvas size in the units a screen-space UI and the mouse use: the window's
         * size in POINTS when there is a window, otherwise the device's size. The drawable
         * is twice that on a Retina display.
         */
        std::pair<int, int> canvasSize() const;

        /// Localization: the locale, its messages and plural forms.
        I18n* i18n() const { return _i18n.get(); }

        /**batcher accessor. */
        BatchManager* batcher() { return _batcher.get(); }

        /// Input devices supplied through AppOptions, or null when the application
        /// asked for none. Engine ends each one's frame in inputUpdate, so the edge
        /// queries (wasPressed / wasReleased) describe the frame that just ran.
        Keyboard* keyboard() const { return _keyboard.get(); }
        Mouse* mouse() const { return _mouse.get(); }
        TouchDevice* touch() const { return _touch.get(); }
        GamePads* gamepads() const { return _gamepads.get(); }
        Controller* controller() const { return _controller.get(); }

        /**
         * Feed one window-system event to every input device that exists.
         *
         * SDL delivers input by pumping a single queue, so there is nothing for a
         * device to subscribe to and the application has to forward what it polls.
         * One call rather than one per device: which devices exist is the engine's
         * business, and a device added later should not need every application to
         * learn about it.
         */
        void handleInputEvent(const SDL_Event& event);

        /// The physics backend supplied through AppOptions, or null when the
        /// application asked for none. RigidBodyComponentSystem drives it.
        PhysicsWorld* physicsWorld() const { return _physicsWorld.get(); }

        /** Async resource loader — single background I/O thread with main-thread callbacks. */
        const std::shared_ptr<ResourceLoader>& loader() const { return _loader; }

        /** Registry owning application assets. */
        const std::shared_ptr<AssetRegistry>& assets() const { return _assets; }

        /// A measured frame time clamped to [0, maxDeltaTime] and multiplied by timeScale:
        /// what update() hands every phase. Public so an application's own per-frame hook
        /// can advance on the same clock.
        float effectiveDeltaTime(float rawDt) const;

    protected:
        virtual double processTimestamp(double timestamp) { return timestamp; }

    private:
        friend MakeTickCallback makeTick(const std::shared_ptr<Engine>& engine);

        static std::unordered_map<std::string, std::shared_ptr<Engine>> _engines;

        // Render a layer composition
        void renderComposition();

        struct PrecompileState
        {
            PrecompileOptions options;
            bool started = false;
            bool sceneStarted = false;   // the frame graph precompiled, with the programs compiling
            bool sceneRetry = false;     // it skipped variants of programs still compiling
            int total = 0;
            int done = -1;
            float progress = -1.0f;
            std::chrono::steady_clock::time_point startTime;
            PrecompileStats stats;
            std::vector<std::function<void(const PrecompileStats&)>> callbacks;
        };
        std::unique_ptr<PrecompileState> _precompile;
        // A frame of the precompile: true when it rendered nothing, false once it finished
        // (and the frame renders as usual).
        bool precompileFrame();
        // Builds and culls the frame as render() does, and precompiles its passes.
        void precompileScene(const PrecompileOptions& options);
        // precompileScene with the statistics counted into the state.
        void precompileScenePass(PrecompileState& state);
        void finishPrecompile();
        // Asks the program library to compile the Slang programs the scene's materials need
        // from source, on compile threads; returns how many it started.
        int requestScenePrograms();

        void tick();

        void initDefaultMaterial();

        void initProgramLibrary();

        // VISUTWIN_SHADER_WATCH: polls the shader source tree and reloads what a change
        // touched, before the frame is built.
        void pollShaderSources();


        void fillFrameStatsBasic(double now, float dt, float ms);

        void fillFrameStats();

        std::shared_ptr<GraphicsDevice> _graphicsDevice;

        // Declared before the root, so it outlives the components the root's teardown
        // destroys (each takes itself out of it on the way).
        std::unique_ptr<ComponentRegistry> _components;

        std::unique_ptr<Entity> _root;
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
        std::shared_ptr<PhysicsWorld> _physicsWorld;

        MakeTickCallback _tick;

        FillMode _fillMode = FillMode::FILLMODE_KEEP_ASPECT;

        ResolutionMode _resolutionMode = ResolutionMode::RESOLUTION_FIXED;

        bool _allowResize = true;

        bool _inFrameUpdate = false;

        double _time = 0.0;

        // Fixed timestep state
        float _fixedDeltaTime = 1.0f / 60.0f;
        double _fixedTimeAccumulator = 0.0;
        double _updatedTime = 0.0;
        int _maxFixedSubSteps = 8;
        float _fixedTimeAlpha = 0.0f;

        bool _renderNextFrame = false;

        bool _destroyRequested = false;

        std::unordered_map<std::string, std::shared_ptr<Entity>> _entityIndex;

        bool _inTools = false;

        // Runtime parity checks for frame lifecycle ordering.
        // True once the frame's stats have been filled. _tick fills them before update/render;
        // apps driving update()/render() directly get them filled from render() instead.
        bool _statsFilledThisFrame = false;
        // Wall-clock time of the previous render(), for deriving frame time on that path.
        double _lastRenderTime = 0.0;

        bool _frameStartCalled = false;
        bool _renderCompositionCalled = false;
        bool _frameEndCalled = false;

        SDL_Window* _window;

        std::unique_ptr<ShaderSourceWatcher> _shaderWatcher;
        bool _shaderWatcherUnavailable = false;
    };

    MakeTickCallback makeTick(const std::shared_ptr<Engine>& engine);
}
