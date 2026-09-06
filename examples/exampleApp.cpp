// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// This is the metal-cpp *_PRIVATE_IMPLEMENTATION translation unit for every
// example. metal-cpp declares its class and selector constants `extern` unless a
// TU defines these macros before including the headers, and the engine library
// deliberately has no such TU — so exactly one file per executable must, and for
// the examples that file is this one.
//
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#endif

#include "exampleApp.h"

#ifdef VISUTWIN_HAS_METAL
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <utility>

#include "cameraControls.h"
#include "framework/constants.h"
#include "log.h"

namespace visutwin::canvas
{
    ExampleApp::ExampleApp(ExampleOptions options)
        : _options(std::move(options))
    {
    }

    ExampleApp::~ExampleApp()
    {
        // Normally a no-op: run() has already torn everything down. This only
        // matters if an example is constructed and never run.
        shutdown();
    }

    int ExampleApp::run()
    {
        log::init();
        if (_options.debugLogging) {
            log::set_level_debug();
        }

        if (!initWindow() || !initEngine()) {
            shutdown();
            return -1;
        }

        if (!create()) {
            spdlog::error("{}: create() failed", _options.title);
            // Whatever create() managed to build before it gave up still has to be
            // released while the engine is alive, so the failure path runs destroy()
            // too — hooks must therefore tolerate a partially built scene.
            destroy();
            shutdown();
            return -1;
        }

        // The scene exists now, so the initialize phase has something to initialize.
        _engine->start();

        const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
        uint64_t previousCounter = SDL_GetPerformanceCounter();

        while (_running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handleEvent(event);
            }

            const uint64_t nowCounter = SDL_GetPerformanceCounter();
            const auto dt = static_cast<float>(
                static_cast<double>(nowCounter - previousCounter) / static_cast<double>(perfFrequency));
            previousCounter = nowCounter;
            _elapsed += dt;

            update(dt);
            _engine->update(dt);
            preRender();
            _engine->render();
            postRender();
        }

        // While the engine and device are still alive — a derived destructor
        // would run too late for anything holding a borrowed pointer to either.
        destroy();

        shutdown();
        return 0;
    }

    bool ExampleApp::initWindow()
    {
        // The backend has to be resolved BEFORE the window exists: Vulkan needs
        // its flag at creation time, and Metal needs a CAMetalLayer off an SDL
        // renderer created for the same window.
        _backend = defaultBackend();

        if (_backend == Backend::Metal) {
            SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
        }

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            spdlog::error("SDL_Init failed: {}", SDL_GetError());
            return false;
        }

        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
        flags |= (_backend == Backend::Vulkan) ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL;

        _window = SDL_CreateWindow(_options.title.c_str(), _options.width, _options.height, flags);
        if (!_window) {
            spdlog::error("SDL_CreateWindow failed: {}", SDL_GetError());
            return false;
        }

        // The SDL renderer exists only to hand back a CAMetalLayer — the Vulkan
        // device builds its own swap chain from the window, and an SDL renderer
        // over a Vulkan window would be a second, competing presenter.
        if (_backend == Backend::Metal) {
            _renderer = SDL_CreateRenderer(_window, nullptr);
            if (!_renderer) {
                spdlog::error("SDL_CreateRenderer failed: {}", SDL_GetError());
                return false;
            }
            SDL_SetRenderVSync(_renderer, SDL_RENDERER_VSYNC_ADAPTIVE);
        }

        return true;
    }

    bool ExampleApp::initEngine()
    {
        GraphicsDeviceOptions deviceOptions;
        deviceOptions.backend = _backend;
        deviceOptions.window = _window;

        if (_backend == Backend::Metal) {
            // SDL hands back the layer as void*, which is what the device takes —
            // no metal-cpp type is needed at the call site.
            deviceOptions.swapChain = SDL_GetRenderMetalLayer(_renderer);
            if (!deviceOptions.swapChain) {
                spdlog::error("SDL_GetRenderMetalLayer returned no layer — is the SDL metal renderer active?");
                return false;
            }
        }

        auto device = createGraphicsDevice(deviceOptions);
        if (!device) {
            spdlog::error("Failed to create the {} graphics device", backendName(_backend));
            return false;
        }
        _device = std::shared_ptr<GraphicsDevice>(std::move(device));

        AppOptions appOptions;
        appOptions.graphicsDevice = _device;

        // The four systems every example uses. Registering an unused one costs a
        // single allocation, which is a better trade than 41 copies of the list.
        appOptions.registerComponentSystem<RenderComponentSystem>();
        appOptions.registerComponentSystem<CameraComponentSystem>();
        appOptions.registerComponentSystem<LightComponentSystem>();
        appOptions.registerComponentSystem<ScriptComponentSystem>();

        configure(appOptions);

        _engine = std::make_shared<Engine>(_window);
        _engine->init(appOptions);
        _engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
        _engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);

        // NOT start() here: the engine's initialize phase runs from start(), and
        // create() has not built the scene yet, so anything it registers would be
        // initialized before it exists — and the first tick would render an empty
        // frame. run() starts the engine once create() has returned, which is the
        // order upstream uses (build the scene, then app.start()).
        return true;
    }

    void ExampleApp::handleEvent(const SDL_Event& event)
    {
        // The example sees every event first, so it can override a default
        // binding (several bind R to something of their own).
        if (onEvent(event)) {
            return;
        }

        switch (event.type) {
        case SDL_EVENT_QUIT:
            _running = false;
            break;

        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_ESCAPE) {
                _running = false;
            } else if (event.key.key == SDLK_R && _cameraControls) {
                _cameraControls->reset();
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            if (_cameraControls) {
                _cameraControls->addZoomInput(event.wheel.y);
            }
            break;

        case SDL_EVENT_PINCH_UPDATE:
            if (_cameraControls) {
                _cameraControls->addZoomInput((event.pinch.scale - 1.0f) * 10.0f);
            }
            break;

        default:
            break;
        }
    }

    void ExampleApp::shutdown()
    {
        // Ordered teardown: the engine owns entities that borrow the device, so
        // it goes first and SDL last.
        _cameraControls = nullptr;
        _engine.reset();
        _device.reset();

        if (_renderer) {
            SDL_DestroyRenderer(_renderer);
            _renderer = nullptr;
        }
        if (_window) {
            SDL_DestroyWindow(_window);
            _window = nullptr;
            SDL_Quit();
        }
    }

    std::string ExampleApp::assetPath(const std::string_view relative)
    {
        std::string path = ASSET_DIR;
        if (!relative.empty() && relative.front() != '/') {
            path += '/';
        }
        path.append(relative);
        return path;
    }

    Entity* ExampleApp::createCamera(const Vector3& position, const Vector3& eulerAngles)
    {
        auto* camera = new Entity();
        camera->setEngine(_engine.get());
        camera->addComponent<CameraComponent>();
        camera->setLocalPosition(position);
        camera->setLocalEulerAngles(eulerAngles.getX(), eulerAngles.getY(), eulerAngles.getZ());
        _engine->root()->addChild(camera);
        return camera;
    }

    CameraControls* ExampleApp::addOrbitControls(Entity* camera, const Vector3& focusPoint)
    {
        if (!camera) {
            return nullptr;
        }

        if (!camera->script()) {
            camera->addComponent<ScriptComponent>();
        }

        auto* controls = camera->script()->create<CameraControls>();
        if (!controls) {
            spdlog::error("Failed to create CameraControls on the camera entity");
            return nullptr;
        }

        controls->setFocusPoint(focusPoint);
        controls->setEnableFly(false);
        controls->storeResetState();

        // Remembered so the default bindings can drive zoom and reset.
        _cameraControls = controls;
        return controls;
    }

    BoundingBox ExampleApp::entityBounds(Entity* entity)
    {
        BoundingBox bounds;
        bounds.setCenter(0.0f, 0.0f, 0.0f);
        bounds.setHalfExtents(0.0f, 0.0f, 0.0f);

        if (!entity) {
            return bounds;
        }

        bool hasAny = false;
        for (auto* render : RenderComponent::instances()) {
            if (!render || !render->entity()) {
                continue;
            }

            auto* owner = render->entity();
            if (owner != entity && !owner->isDescendantOf(entity)) {
                continue;
            }

            for (auto* meshInstance : render->meshInstances()) {
                if (!meshInstance) {
                    continue;
                }
                bounds.add(meshInstance->aabb());
                hasAny = true;
            }
        }

        if (!hasAny) {
            bounds.setCenter(entity->position());
            bounds.setHalfExtents(0.5f, 0.5f, 0.5f);
        }
        return bounds;
    }

    Entity* ExampleApp::createPrimitive(const char* type, Material* material,
        const Vector3& position, const Vector3& scale, const std::vector<int>& layers)
    {
        auto* entity = new Entity();
        entity->setEngine(_engine.get());
        entity->setLocalPosition(position);
        entity->setLocalScale(scale.getX(), scale.getY(), scale.getZ());

        if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
            render->setMaterial(material);
            render->setType(type);
            if (!layers.empty()) {
                render->setLayers(layers);
            }
        }

        _engine->root()->addChild(entity);
        return entity;
    }

    Entity* ExampleApp::createDirectionalLight(const Vector3& eulerAngles, const Color& color,
        const float intensity, const bool castShadows)
    {
        auto* light = new Entity();
        light->setEngine(_engine.get());

        if (auto* component = static_cast<LightComponent*>(light->addComponent<LightComponent>())) {
            component->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            component->setColor(color);
            component->setIntensity(intensity);
            component->setCastShadows(castShadows);
        }

        light->setLocalEulerAngles(eulerAngles.getX(), eulerAngles.getY(), eulerAngles.getZ());
        _engine->root()->addChild(light);
        return light;
    }
}
