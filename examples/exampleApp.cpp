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

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "extras/script/cameraControls.h"
#include "framework/constants.h"
#include "scene/materials/standardMaterial.h"
#include "framework/extras/miniStats.h"
#include "core/log.h"
#include "overlay/imguiOverlay.h"

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

        // VISUTWIN_AMBIENT_SH=r,g,b puts a UNIFORM SH light probe of that radiance on
        // the scene, which switches every forward variant to VT_FEATURE_LIGHT_PROBES
        // without touching the example. A uniform probe is the one whose diffuse is
        // known exactly (a flat ambient of r,g,b — the SH constants are folded in),
        // so what it isolates is everything ELSE the probe path changes: no example
        // in the tree sets probes, and this is how the Vulkan chunk was shown to drop
        // the environment specular under them (2026-09-19).
        if (const char* sh = std::getenv("VISUTWIN_AMBIENT_SH"); sh && *sh) {
            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (std::sscanf(sh, "%f,%f,%f", &r, &g, &b) == 3) {
                std::array<Vector3, 9> coefficients{};
                coefficients[0] = Vector3(r, g, b);
                scene()->setAmbientSH(coefficients);
                spdlog::info("Ambient SH probe: uniform ({}, {}, {}) from VISUTWIN_AMBIENT_SH", r, g, b);
            } else {
                spdlog::warn("VISUTWIN_AMBIENT_SH='{}' is not r,g,b; ignored", sh);
            }
        }

        // VISUTWIN_SSR_FLOOR=y,size[,ssr[,pole[,gloss]]] lays a mirror-like metal plane of that size at
        // height y under any example and asks every camera for the scene colour and
        // depth grabs, with screen-space reflections on the plane unless the third
        // field is 0. No upstream example drives SSR, so this is how the path is
        // exercised: the same scene with the third field 0 is the control, and the
        // difference between the two frames is the reflection alone. Found the camera
        // frame's missing depth grab on 2026-09-19 (SSR under post-processing did
        // nothing on either backend).
        if (const char* floor = std::getenv("VISUTWIN_SSR_FLOOR"); floor && *floor) {
            float y = 0.0f, size = 100.0f;
            int ssr = 1;
            // Optional fourth field: a magenta pole standing on the floor at x = pole,
            // z = 0, height size / 10, 0.3 x height wide. Its reflection must be collinear with it on
            // screen — a vertical world line and its mirror image share one world
            // line — so any sideways offset in the reflection is a march error.
            float pole = 0.0f;
            // Optional fifth field: the floor's gloss (default 0.98, a mirror). Lower it
            // to see the roughness cone blur the reflection while the pillar stays sharp.
            float gloss = 0.98f;
            const int fields = std::sscanf(floor, "%f,%f,%d,%f,%f", &y, &size, &ssr, &pole, &gloss);
            if (fields >= 2) {
                auto* material = new StandardMaterial();
                material->setName("ssr-floor");
                // A MIRROR: a metal's reflectance is its base colour, so a dark metal
                // reflects at a tenth and hides anything but the brightest emitters
                // (the first version of this floor had albedo 0.1 and "lost" a test
                // pillar that the march was hitting all along).
                material->setDiffuse(Color(0.95f, 0.95f, 0.95f));
                material->setGloss(std::clamp(gloss, 0.0f, 1.0f));
                material->setMetalness(1.0f);
                material->setUseMetalness(true);
                material->setUseScreenSpaceReflection(ssr != 0);
                createPrimitive("plane", material, Vector3(0.0f, y, 0.0f), Vector3(size, 1.0f, size));
                if (fields >= 4) {
                    auto* poleMaterial = new StandardMaterial();
                    poleMaterial->setName("ssr-pole");
                    poleMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f));
                    poleMaterial->setEmissive(Color(1.0f, 0.0f, 1.0f));
                    const float height = size / 10.0f;
                    createPrimitive("box", poleMaterial, Vector3(pole, y + height * 0.5f, 0.0f),
                        Vector3(height * 0.3f, height, height * 0.3f));
                }
                int cameras = 0;
                for (GraphNode* node : _engine->root()->find([](GraphNode* n) {
                        auto* e = dynamic_cast<Entity*>(n);
                        return e && e->findComponent<CameraComponent>() != nullptr; })) {
                    auto* camera = static_cast<Entity*>(node)->findComponent<CameraComponent>();
                    camera->requestSceneColorMap(true);
                    camera->requestSceneDepthMap(true);
                    ++cameras;
                }
                spdlog::info("SSR floor: y {} size {} ssr {} gloss {} from VISUTWIN_SSR_FLOOR; grabs requested on {} camera(s)",
                    y, size, ssr, gloss, cameras);
            } else {
                spdlog::warn("VISUTWIN_SSR_FLOOR='{}' is not y,size[,ssr]; ignored", floor);
            }
        }

        // VISUTWIN_FILL_LIGHT=pitch,yaw,intensity[,shadows] adds a second, white
        // directional light aimed by those Euler angles (degrees). Only one directional
        // shadow exists per layer, so this light must come out UNSHADOWED however the
        // key light is set up — with shadows=1 too, since it is created after the
        // example's own light and so loses the one slot. Aim it like the key light and
        // the difference from a run without it is the fill alone, which must be as
        // bright inside the key light's shadow as outside it. No example has two
        // directional lights; this is how Vulkan was found shadowing a shadowless fill
        // with the key light's map (2026-09-23).
        if (const char* fill = std::getenv("VISUTWIN_FILL_LIGHT"); fill && *fill) {
            float pitch = 0.0f, yaw = 0.0f, intensity = 1.0f;
            int shadows = 0;
            if (std::sscanf(fill, "%f,%f,%f,%d", &pitch, &yaw, &intensity, &shadows) >= 3) {
                createDirectionalLight(Vector3(pitch, yaw, 0.0f), Color(1.0f, 1.0f, 1.0f),
                    intensity, shadows != 0);
                spdlog::info("Fill light: euler ({}, {}) intensity {} shadows {} from VISUTWIN_FILL_LIGHT",
                    pitch, yaw, intensity, shadows);
            } else {
                spdlog::warn("VISUTWIN_FILL_LIGHT='{}' is not pitch,yaw,intensity[,shadows]; ignored", fill);
            }
        }

        // VISUTWIN_LOCAL_LIGHT=x,y,z,intensity,range adds a white OMNI light, which under
        // the default clustered lighting goes through the cluster loop rather than the
        // main light array. No example puts a local light on a clearcoat, sheen,
        // iridescent or Oren-Nayar material, so this is how that loop's material terms
        // are driven; a run without it is the control, and the difference between the
        // two frames is the light's contribution alone. Found the Vulkan cluster loop
        // missing all four terms (2026-09-23).
        if (const char* local = std::getenv("VISUTWIN_LOCAL_LIGHT"); local && *local) {
            float x = 0.0f, y = 0.0f, z = 0.0f, intensity = 1.0f, range = 10.0f;
            if (std::sscanf(local, "%f,%f,%f,%f,%f", &x, &y, &z, &intensity, &range) == 5) {
                auto* lightEntity = new Entity();
                lightEntity->setName("local-light");
                lightEntity->setEngine(_engine.get());
                _engine->root()->addChild(lightEntity);
                lightEntity->setLocalPosition(x, y, z);
                auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
                light->setType(LightType::LIGHTTYPE_OMNI);
                light->setColor(Color(1.0f, 1.0f, 1.0f));
                light->setIntensity(intensity);
                light->setRange(range);
                spdlog::info("Local light: omni at ({}, {}, {}) intensity {} range {} from VISUTWIN_LOCAL_LIGHT",
                    x, y, z, intensity, range);
            } else {
                spdlog::warn("VISUTWIN_LOCAL_LIGHT='{}' is not x,y,z,intensity,range; ignored", local);
            }
        }

        // The scene exists now, so the initialize phase has something to initialize.
        _engine->start();

        const uint64_t perfFrequency = SDL_GetPerformanceFrequency();
        uint64_t previousCounter = SDL_GetPerformanceCounter();

        // VISUTWIN_FIXED_DT=seconds replaces the measured frame time with a constant,
        // so an animated example reaches the SAME state at the same frame in every
        // run and two builds can be screenshot-diffed (VISUTWIN_SCREENSHOT_FRAME).
        // Without it, frame N lands wherever the wall clock put it.
        float fixedDt = 0.0f;
        if (const char* value = std::getenv("VISUTWIN_FIXED_DT"); value && *value) {
            fixedDt = std::max(std::strtof(value, nullptr), 0.0f);
            spdlog::info("Fixed frame time {} s from VISUTWIN_FIXED_DT", fixedDt);
        }

        while (_running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handleEvent(event);
            }

            const uint64_t nowCounter = SDL_GetPerformanceCounter();
            const auto dt = fixedDt > 0.0f ? fixedDt : static_cast<float>(
                static_cast<double>(nowCounter - previousCounter) / static_cast<double>(perfFrequency));
            previousCounter = nowCounter;
            _elapsed += dt;

            // A click on the HUD is the HUD's (it switches its view) and must not also
            // orbit the camera. Upstream's panel is a DOM element over the canvas, so a
            // pointer on it never reaches the canvas at all; this is that rule scoped to
            // the camera controls, the one consumer of a bare click. Consulted only while
            // the HUD draws: ImGui refreshes its answer in NewFrame, and a hidden HUD would
            // leave the last answer standing.
            if (_cameraControls) {
                _cameraControls->setInputBlocked(
                    _overlay && _miniStats && _miniStats->enabled() && _overlay->wantCaptureMouse());
            }

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
            if (std::getenv("VT_NOVSYNC")) deviceOptions.vsync = false;   // PROBE temporary
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

        // Real input devices, which the engine then feeds from the event loop and
        // ends the frame for. Every example gets them: the camera controls read the
        // keyboard and mouse through the engine rather than polling SDL from here,
        // and an example wanting a gamepad or a named action already has one.
        appOptions.keyboard = std::make_shared<Keyboard>();
        appOptions.mouse = std::make_shared<Mouse>();
        appOptions.touch = std::make_shared<TouchDevice>();
        appOptions.gamepads = std::make_shared<GamePads>();

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

        // The performance HUD. Upstream's example harness puts ministats on every
        // example, so it belongs to the host here rather than to any one scene —
        // and MiniStats hooks "postrender", which is the only place either backend
        // can still reach the back buffer.
        //
        // Suppressed while VISUTWIN_SCREENSHOT is armed. The capture happens inside
        // frameEnd, AFTER the hook the HUD draws on, so every parity screenshot
        // would otherwise carry a translucent window over the top-left of the frame
        // — exactly the region the upstream comparisons sample.
        // VISUTWIN_MINISTATS=0/1 overrides that either way — which is also the only
        // way to capture a screenshot WITH the HUD in it, since the capture and the
        // suppression key off the same variable. VISUTWIN_MINISTATS=detailed opens
        // it in the detailed view, the one a screenshot cannot click its way to.
        const char* screenshotPath = std::getenv("VISUTWIN_SCREENSHOT");
        const bool screenshotArmed = screenshotPath && *screenshotPath;
        const char* hudOverride = std::getenv("VISUTWIN_MINISTATS");
        const bool wantHud = (hudOverride && *hudOverride)
            ? (*hudOverride != '0')
            : !screenshotArmed;
        if (wantHud) {
            _overlay = std::make_unique<ImGuiOverlay>();
            _overlay->init(_device.get(), _window);
            if (_overlay->isInitialized()) {
                _miniStats = std::make_unique<MiniStats>(_engine, _overlay.get());
                if (hudOverride && std::string_view(hudOverride) == "detailed") {
                    _miniStats->setDetailed(true);
                }
            } else {
                // init() has already logged the reason. Drop the overlay rather
                // than keep a dead one that every frame would have to test.
                _overlay.reset();
            }
        }

        // NOT start() here: the engine's initialize phase runs from start(), and
        // create() has not built the scene yet, so anything it registers would be
        // initialized before it exists — and the first tick would render an empty
        // frame. run() starts the engine once create() has returned, which is the
        // order upstream uses (build the scene, then app.start()).
        return true;
    }

    void ExampleApp::handleEvent(const SDL_Event& event)
    {
        // Devices first, and unconditionally: an example that consumes an event
        // must not leave the keyboard believing a key is still held. They record
        // state and fire their own events; they do not consume anything.
        if (_engine) {
            _engine->handleInputEvent(event);
        }

        // The HUD sees every event too, and CONSUMES none: the camera controls and
        // each example's own bindings have to keep working while it is on screen.
        // The one exception, a click landing ON the panel, is handled in run() by
        // blocking the camera controls for that frame rather than by eating events.
        if (_overlay) {
            _overlay->processEvent(event);
        }

        // The example sees every event next, so it can override a default
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
            } else if (event.key.key == SDLK_F1 && _miniStats) {
                // F1 rather than a letter: every letter worth having is already an
                // example's own binding somewhere in the set.
                _miniStats->setEnabled(!_miniStats->enabled());
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

        // The HUD goes first and in this order: MiniStats unhooks itself from the
        // engine's "postrender" event, and the overlay's shutdown still needs the
        // device — the Vulkan path waits the device idle before freeing ImGui's
        // font texture and pipeline, which it cannot do once _device is gone.
        _miniStats.reset();
        _overlay.reset();

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
