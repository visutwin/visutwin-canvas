// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 18.07.2025.
//
#include "engine.h"

#include <cassert>

#include "batching/batchManager.h"
#include "components/componentSystemRegistry.h"
#include "scene/materials/standardMaterial.h"
#include "scene/shader-lib/programLibrary.h"
#include "framework/components/componentSystem.h"
#include "framework/components/script/scriptComponentSystem.h"
#include "framework/assets/asset.h"
#include "scene/frameGraph.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    std::unordered_map<std::string, std::shared_ptr<Engine>> Engine::_engines;

    MakeTickCallback makeTick(const std::shared_ptr<Engine>& app) {
        return [app](double timestamp, void* xrFrame) {
            if (!app || !app->_graphicsDevice) {
                return;
            }

            // Cancel any hanging request
            if (app->_frameRequestId) {
                app->_frameRequestId = nullptr;
            }

            app->_inFrameUpdate = true;

            double currentTime = app->processTimestamp(timestamp);
            if (currentTime == 0) {
                auto now = std::chrono::high_resolution_clock::now();
                currentTime = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
            }

            double ms = currentTime - (app->_time > 0 ? app->_time : currentTime);
            float dt = static_cast<float>(ms / 1000.0);
            dt = std::clamp(dt, 0.0f, app->_maxDeltaTime);
            dt *= app->_timeScale;

            app->_time = currentTime;

            // Submit a request to queue up a new animation frame
            if (app->_xr && app->_xr->active()) {
                app->_frameRequestId = app->_xr->requestAnimationFrame(app->_tick);
            } else {
                // Would use platform-specific requestAnimationFrame
                app->_frameRequestId = reinterpret_cast<void*>(1); // Placeholder
            }

            if (app->_graphicsDevice->contextLost()) {
                app->_inFrameUpdate = false;
                return;
            }

            app->fillFrameStatsBasic(currentTime, dt, static_cast<float>(ms));
            app->fillFrameStats();

            app->fire("frameupdate", ms);

            bool skipUpdate = false;

            if (xrFrame && app->_xr) {
                skipUpdate = !app->_xr->update(xrFrame);
            }

            if (!skipUpdate) {
                app->update(dt);

                app->fire("framerender");

                if (app->_autoRender || app->_renderNextFrame) {
                    app->render();
                    app->_renderNextFrame = false;
                }

                app->fire("frameend");
            }

            app->_inFrameUpdate = false;

            if (app->_destroyRequested) {
                app->destroy();
            }
        };
    }

    Engine::~Engine()
    {
        destroy();
    }

    void Engine::destroy()
    {
        if (_inFrameUpdate) {
            _destroyRequested = true;
            return;
        }

        fire("destroy");

        // Cleanup root entity
        if (_root) {
            _root.reset();
        }

        // Cleanup input devices
        if (_mouse) {
            _mouse->detach();
            _mouse.reset();
        }

        if (_keyboard) {
            _keyboard->detach();
            _keyboard.reset();
        }

        if (_touch) {
            _touch->detach();
            _touch.reset();
        }

        if (_elementInput) {
            _elementInput->detach();
            _elementInput.reset();
        }

        if (_gamepads) {
            _gamepads.reset();
        }

        // Cleanup systems
        if (_systems) {
            _systems.reset();
        }

        // Cleanup assets
        if (_assets) {
            for (auto assetList = _assets->list(); auto& asset : assetList) {
                asset->unload();
            }
            _assets.reset();
        }

        // Cleanup other components
        if (_bundles) {
            _bundles.reset();
        }

        if (_i18n) {
            _i18n.reset();
        }

        if (_loader) {
            _loader->shutdown();
            _loader.reset();
        }

        if (_scene) {
            _scene.reset();
        }

        if (_scripts) {
            _scripts.reset();
        }

        if (_scenes) {
            _scenes.reset();
        }

        if (_lightmapper) {
            _lightmapper.reset();
        }

        if (_batcher) {
            _batcher.reset();
        }

        _entityIndex.clear();

        if (_xr) {
            _xr->end();
            _xr.reset();
        }

        if (_renderer) {
            _renderer.reset();
        }

        if (_graphicsDevice) {
            _graphicsDevice.reset();
        }

        _tick = nullptr;

        // Remove from the applications registry
        _engines.clear();
    }

    void Engine::init(const AppOptions& appOptions)
    {
        _graphicsDevice = appOptions.graphicsDevice;
        if (!_graphicsDevice) {
            throw std::runtime_error("The application cannot be created without a valid GraphicsDevice");
        }

        _root = std::make_shared<Entity>();
        _root->setEngine(this);
        // The root entity has no parent, so _enabledInHierarchy must be set
        // explicitly — onInsertChild never runs for it.
        // Matches upstream: `this.root._enabledInHierarchy = true;`
        _root->setEnabledInHierarchy(true);

        Asset::setDefaultGraphicsDevice(_graphicsDevice);

        initDefaultMaterial();
        initProgramLibrary();

        _stats = std::make_shared<ApplicationStats>(_graphicsDevice);
        _scene = std::make_shared<Scene>(_graphicsDevice);
        registerSceneImmediate(_scene);

        _loader = std::make_shared<ResourceLoader>(shared_from_this());
        _loader->addHandler(AssetType::TEXTURE,   std::make_unique<TextureResourceHandler>());
        _loader->addHandler(AssetType::CONTAINER,  std::make_unique<ContainerResourceHandler>());
        _loader->addHandler(AssetType::FONT,       std::make_unique<FontResourceHandler>());
        _assets = std::make_shared<AssetRegistry>(_loader);
        _bundles = std::make_shared<BundleRegistry>(_assets);
        _scenes = std::make_shared<SceneRegistry>(shared_from_this());
        _scripts = std::make_shared<ScriptRegistry>(shared_from_this());

        _systems = std::make_shared<ComponentSystemRegistry>();
        for (auto componentSystem : appOptions.componentSystems)
        {
            _systems->add(componentSystem(this));
        }

        _i18n = std::make_shared<I18n>(shared_from_this());

        _scriptsOrder = appOptions.scriptsOrder;

        // Create default layers
        _defaultLayerWorld = std::make_shared<Layer>("World", 1);
        _defaultLayerDepth = std::make_shared<Layer>("Depth", 2);
        _defaultLayerSkybox = std::make_shared<Layer>("Skybox", 3);
        _defaultLayerUi = std::make_shared<Layer>("UI", 4);
        _defaultLayerImmediate = std::make_shared<Layer>("Immediate", 5);

        // Create default layer composition
        auto defaultLayerComposition = std::make_shared<LayerComposition>("default");
        defaultLayerComposition->pushOpaque(_defaultLayerWorld);
        defaultLayerComposition->pushOpaque(_defaultLayerDepth);
        defaultLayerComposition->pushOpaque(_defaultLayerSkybox);
        defaultLayerComposition->pushTransparent(_defaultLayerWorld);
        defaultLayerComposition->pushOpaque(_defaultLayerImmediate);
        defaultLayerComposition->pushTransparent(_defaultLayerImmediate);
        defaultLayerComposition->pushTransparent(_defaultLayerUi);
        _scene->setLayers(defaultLayerComposition);

        _renderer = std::make_shared<ForwardRenderer>(_graphicsDevice, _scene);

        if (appOptions.lightmapper) {
            _lightmapper = appOptions.lightmapper;
        }

        if (appOptions.batchManager) {
            _batcher = appOptions.batchManager;
        } else {
            _batcher = std::make_shared<BatchManager>(_graphicsDevice.get());
        }

        _keyboard = appOptions.keyboard;
        _mouse = appOptions.mouse;
        _touch = appOptions.touch;
        _gamepads = appOptions.gamepads;
        _elementInput = appOptions.elementInput;
        if (_elementInput) {
            _elementInput->setEngine(shared_from_this());
        }

        _xr = appOptions.xr;
        _scriptPrefix = appOptions.scriptPrefix;

        // Create a tick function
        _tick = makeTick(shared_from_this());
    }

    void Engine::initDefaultMaterial()
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setName("Default Material");
        setDefaultMaterial(_graphicsDevice, material);
    }

    void Engine::initProgramLibrary()
    {
        auto library = std::make_shared<ProgramLibrary>(_graphicsDevice);
        setProgramLibrary(_graphicsDevice, library);
    }

    void Engine::start()
    {
        _frame = 0;
        tick();
    }

    void Engine::render()
    {
        assert(_graphicsDevice && "Engine::render requires a valid graphics device");
        _frameStartCalled = false;
        _renderCompositionCalled = false;
        _frameEndCalled = false;

        _graphicsDevice->frameStart();
        _frameStartCalled = true;
        renderComposition();
        _renderCompositionCalled = true;
        fire("postrender");

        if (_graphicsDevice->insideRenderPass()) {
            spdlog::error("Frame parity violation: render() reached frameEnd while still inside a render pass");
            assert(!_graphicsDevice->insideRenderPass() && "Unbalanced render pass before frameEnd");
        }

        _graphicsDevice->frameEnd();
        _frameEndCalled = true;

        if (!(_frameStartCalled && _renderCompositionCalled && _frameEndCalled)) {
            spdlog::error("Frame parity violation: expected frameStart -> render passes -> frameEnd sequence");
            assert(false && "Invalid frame lifecycle ordering");
        }
    }

    void Engine::tick()
    {
        if (_tick) {
            auto now = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(now.time_since_epoch()).count();
            _tick(ms, nullptr);
        }
    }

    void Engine::renderComposition()
    {
        if (!_frameStartCalled || _frameEndCalled) {
            spdlog::error("Frame parity violation: renderComposition called outside active frame scope");
            assert(false && "renderComposition must run after frameStart and before frameEnd");
        }

        if (!_renderer || !_scene || !_graphicsDevice) {
            return;
        }

        const auto& layerComposition = _scene->layers();
        if (!layerComposition) {
            return;
        }

        FrameGraph frameGraph;
        _renderer->buildFrameGraph(&frameGraph, layerComposition.get());
        frameGraph.render(_graphicsDevice.get());
    }

    void Engine::registerSceneImmediate(const std::shared_ptr<Scene>& scene) {
        if (_scene && _scene->immediate()) {
            on("postrender", [scene]() {
                scene->immediate()->onPostRender();
            });
        }
    }

    void Engine::fillFrameStatsBasic(double now, float dt, float ms)
    {
        if (_stats) {
            auto& stats = _stats->frame();
            stats.dt = dt;
            stats.ms = ms;
            if (now > stats.timeToCountFrames) {
                stats.fps = stats.fpsAccum;
                stats.fpsAccum = 0;
                stats.timeToCountFrames = now + 1000;
            } else {
                stats.fpsAccum++;
            }

            _stats->drawCalls().total = _graphicsDevice->drawCallsPerFrame();
            _graphicsDevice->resetDrawCallsPerFrame();

            stats.gsplats = _renderer->_gsplatCount;
        }
    }

    void Engine::setCanvasFillMode(FillMode mode, int width, int height)
    {
        _fillMode = mode;
        resizeCanvas(width, height);
    }

    std::pair<int, int> Engine::resizeCanvas(int width, int height)
    {
        if (!_allowResize) {
            return {0, 0};
        }

        // Prevent resizing when in XR session
        if (_xr && _xr->active()) {
            return {0, 0};
        }

        // Get window dimensions (simplified)
        auto windowSize = _graphicsDevice->size();   // Would get from an actual window

        if (_fillMode == FillMode::FILLMODE_KEEP_ASPECT) {
            float r = static_cast<float>(windowSize.first) / windowSize.second;
            float winR = static_cast<float>(windowSize.first) / windowSize.second;

            if (r > winR) {
                width = windowSize.first;
                height = static_cast<int>(width / r);
            } else {
                height = windowSize.second;
                width = static_cast<int>(height * r);
            }
        } else if (_fillMode == FillMode::FILLMODE_FILL_WINDOW) {
            width = windowSize.first;
            height = windowSize.second;
        }

        // Set canvas style (would interact with actual canvas)
        updateCanvasSize();

        return {width, height};
    }

    void Engine::setCanvasResolution(ResolutionMode mode, int width, int height)
    {
        _resolutionMode = mode;

        // In AUTO mode the resolution is the same as the canvas size, unless specified
        if (mode == ResolutionMode::RESOLUTION_AUTO && (width == 0)) {
            auto size = _graphicsDevice->size();
            width = size.first;
            height = size.second;
        }

        _graphicsDevice->resizeCanvas(width, height);
    }

    void Engine::fillFrameStats()
    {
        auto& stats = _stats->frame();

        // Render stats
        stats.cameras = _renderer->_camerasRendered;
        stats.materials = _renderer->_materialSwitches;
        stats.shaders = _graphicsDevice->_shaderSwitchesPerFrame;
        stats.shadowMapUpdates = _renderer->_shadowMapUpdates;
        stats.shadowMapTime = _renderer->_shadowMapTime;
        stats.depthMapTime = _renderer->_depthMapTime;
        stats.forwardTime = _renderer->_forwardTime;

        auto& prims = _graphicsDevice->_primsPerFrame;
        if (prims.size() <= static_cast<size_t>(PRIMITIVE_TRIFAN)) {
            prims.resize(static_cast<size_t>(PRIMITIVE_TRIFAN) + 1, 0);
        }
        stats.triangles = prims[PRIMITIVE_TRIANGLES] / 3 +
            std::max(prims[PRIMITIVE_TRISTRIP] - 2, 0) +
            std::max(prims[PRIMITIVE_TRIFAN] - 2, 0);

        stats.cullTime = _renderer->_cullTime;
        stats.sortTime = _renderer->_sortTime;
        stats.skinTime = _renderer->_skinTime;
        stats.morphTime = _renderer->_morphTime;
        stats.lightClusters = _renderer->_lightClusters;
        stats.lightClustersTime = _renderer->_lightClustersTime;
        stats.otherPrimitives = 0;

        for (int i = 0; i < prims.size(); i++) {
            if (i < PRIMITIVE_TRIANGLES) {
                stats.otherPrimitives += prims[i];
            }
            prims[i] = 0;
        }

        _renderer->_camerasRendered = 0;
        _renderer->_materialSwitches = 0;
        _renderer->_shadowMapUpdates = 0;
        _graphicsDevice->_shaderSwitchesPerFrame = 0;
        _renderer->_cullTime = 0;
        _renderer->_layerCompositionUpdateTime = 0;
        _renderer->_lightClustersTime = 0;
        _renderer->_sortTime = 0;
        _renderer->_skinTime = 0;
        _renderer->_morphTime = 0;
        _renderer->_shadowMapTime = 0;
        _renderer->_depthMapTime = 0;
        _renderer->_forwardTime = 0;

        // Draw call stats
        auto& drawCallstats = _stats->drawCalls();
        drawCallstats.forward = _renderer->_forwardDrawCalls;
        drawCallstats.depth = 0;
        drawCallstats.shadow = _renderer->_shadowDrawCalls;
        drawCallstats.skinned = _renderer->_skinDrawCalls;
        drawCallstats.immediate = 0;
        drawCallstats.instanced = 0;
        drawCallstats.removedByInstancing = 0;
        drawCallstats.misc = drawCallstats.total - (drawCallstats.forward + drawCallstats.shadow);

        _renderer->_shadowDrawCalls = 0;
        _renderer->_forwardDrawCalls = 0;
        _renderer->_numDrawCallsCulled = 0;
        _renderer->_skinDrawCalls = 0;
        _renderer->_instancedDrawCalls = 0;

        _stats->misc().renderTargetCreationTime = _graphicsDevice->_renderTargetCreationTime;

        auto& particleStats = _stats->particles();
        particleStats.updatesPerFrame = particleStats._updatesPerFrame;
        particleStats.frameTime = particleStats._frameTime;
        particleStats._updatesPerFrame = 0;
        particleStats._frameTime = 0;
    }

    void Engine::inputUpdate(float dt)
    {
        if (_controller) {
            _controller->update();
        }
        if (_mouse) {
            _mouse->update();
        }
        if (_keyboard) {
            _keyboard->update();
        }
        if (_gamepads) {
            _gamepads->update();
        }
    }

    void Engine::update(float dt)
    {
        _frame++;

        if (_stats) {
            _stats->frame().fixedUpdateTime = 0.0;
        }

        _graphicsDevice->update();

        // Dispatch completed async resource load callbacks on the main thread.
        // Limit to 1 per frame so heavy callbacks (e.g. 70 MB GLB parsing)
        // don't stall the event loop and cause a spinning-wait cursor.
        if (_loader) {
            _loader->processCompletions(1);
        }

        auto updateStart = std::chrono::high_resolution_clock::now();
        bool hasScriptSystem = false;
        bool scriptUpdateCalled = false;
        bool scriptPostUpdateCalled = false;

        _systems->fire(_inTools ? "toolsUpdate" : "update", dt);
        _systems->fire("animationUpdate", dt);

        if (auto* scriptSystemBase = _systems->getByComponentType<ScriptComponent>()) {
            if (auto* scriptSystem = dynamic_cast<ScriptComponentSystem*>(scriptSystemBase)) {
                hasScriptSystem = true;
                scriptSystem->update(dt);
                scriptUpdateCalled = true;
            }
        }

        _systems->fire("postUpdate", dt);

        if (auto* scriptSystemBase = _systems->getByComponentType<ScriptComponent>()) {
            if (auto* scriptSystem = dynamic_cast<ScriptComponentSystem*>(scriptSystemBase)) {
                hasScriptSystem = true;
                if (!scriptUpdateCalled) {
                    spdlog::error("Script lifecycle parity violation: postUpdateScripts called before updateScripts");
                    assert(scriptUpdateCalled && "Script postUpdate invoked before script update");
                }
                scriptSystem->postUpdate(dt);
                scriptPostUpdateCalled = true;
            }
        }

        if (hasScriptSystem && !(scriptUpdateCalled && scriptPostUpdateCalled)) {
            spdlog::error("Script lifecycle parity violation: expected script update + postUpdate each frame");
            assert(false && "Incomplete script lifecycle in frame update");
        }

        // Update dynamic batch matrix palettes and AABBs.
        // called per frame after
        // scripts have updated transforms (FK, animation, etc.).
        if (_batcher) {
            _batcher->updateAll();
        }

        fire("update", dt);

        // Fixed timestep accumulator — run simulation substeps at constant rate
        _fixedTimeAccumulator += dt;
        int substeps = 0;
        while (_fixedTimeAccumulator >= _fixedDeltaTime
               && substeps < _maxFixedSubSteps) {
            fixedUpdate(_fixedDeltaTime);
            _fixedTimeAccumulator -= _fixedDeltaTime;
            substeps++;
        }
        // Clamp accumulator to prevent unbounded growth if substep cap was hit
        if (_fixedTimeAccumulator > _fixedDeltaTime) {
            _fixedTimeAccumulator = _fixedDeltaTime;
        }
        _fixedTimeAlpha = _fixedDeltaTime > 0.0f
            ? static_cast<float>(_fixedTimeAccumulator / _fixedDeltaTime)
            : 0.0f;

        inputUpdate(dt);

        auto updateEnd = std::chrono::high_resolution_clock::now();
        auto updateTime = std::chrono::duration<float, std::milli>(updateEnd - updateStart).count();

        if (_stats) {
            _stats->frame().updateTime = updateTime;
        }
    }

    void Engine::fixedUpdate(float fixedDt)
    {
        auto fixedStart = std::chrono::high_resolution_clock::now();

        _systems->fire("fixedUpdate", fixedDt);

        if (auto* scriptSystemBase = _systems->getByComponentType<ScriptComponent>()) {
            if (auto* scriptSystem = dynamic_cast<ScriptComponentSystem*>(scriptSystemBase)) {
                scriptSystem->fixedUpdate(fixedDt);
            }
        }

        fire("fixedUpdate", fixedDt);

        auto fixedEnd = std::chrono::high_resolution_clock::now();
        if (_stats) {
            _stats->frame().fixedUpdateTime +=
                std::chrono::duration<double, std::milli>(fixedEnd - fixedStart).count();
        }
    }

    void Engine::updateCanvasSize()
    {
        // Don't update if we are in VR or XR
        if ((!_allowResize) || (_xr != nullptr && _xr->active())) {
            return;
        }

        // In AUTO mode the resolution is changed to match the canvas size
        if (_resolutionMode == ResolutionMode::RESOLUTION_AUTO) {
            int w, h;
            SDL_GetWindowSizeInPixels(_window, &w, &h);
            _graphicsDevice->resizeCanvas(w, h);
        }
    }
}
