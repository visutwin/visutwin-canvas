// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include <SDL3/SDL.h>
#include <QuartzCore/QuartzCore.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "framework/engine.h"
#include "log.h"
#include "core/math/quaternion.h"
#include "framework/appOptions.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/light/lightComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "scene/materials/standardMaterial.h"

constexpr int WINDOW_WIDTH = 1200;
constexpr int WINDOW_HEIGHT = 760;

using namespace visutwin::canvas;

SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;

void setLookAt(Entity* entity, const Vector3& target)
{
    if (!entity) {
        return;
    }

    const Vector3 position = entity->position();
    const Vector3 lookDir = (target - position).normalized();
    const float pitchDeg = std::asin(std::clamp(lookDir.getY(), -1.0f, 1.0f)) * RAD_TO_DEG;
    const float yawDeg = std::atan2(-lookDir.getX(), -lookDir.getZ()) * RAD_TO_DEG;
    entity->setLocalEulerAngles(pitchDeg, yawDeg, 0.0f);
}

void orientYAxisToDirection(Entity* entity, const Vector3& direction)
{
    if (!entity || direction.lengthSquared() < 1e-8f) {
        return;
    }

    const Vector3 from = Vector3(0.0f, 1.0f, 0.0f);
    const Vector3 to = direction.normalized();
    const float dot = std::clamp(from.dot(to), -1.0f, 1.0f);

    Quaternion rotation;
    if (dot > 0.9999f) {
        rotation = Quaternion::fromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 0.0f);
    } else if (dot < -0.9999f) {
        rotation = Quaternion::fromAxisAngle(Vector3(1.0f, 0.0f, 0.0f), 180.0f);
    } else {
        const Vector3 axis = from.cross(to).normalized();
        const float angleDeg = std::acos(dot) * RAD_TO_DEG;
        rotation = Quaternion::fromAxisAngle(axis, angleDeg);
    }

    entity->setLocalRotation(rotation);
}

Entity* createPrimitiveEntity(
    Engine* engine,
    const std::string& type,
    const Vector3& position,
    const Vector3& scale,
    StandardMaterial* material)
{
    auto* entity = new Entity();
    entity->setEngine(engine);
    entity->setLocalPosition(position);
    entity->setLocalScale(scale);

    auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
    if (render) {
        render->setType(type);
        render->setMaterial(material);
    }

    engine->root()->addChild(entity);
    return entity;
}

Entity* createPhysicalShape(
    Engine* engine,
    const std::string& type,
    StandardMaterial* material,
    const Vector3& position)
{
    auto* entity = createPrimitiveEntity(engine, type, position, Vector3(1.0f, 1.0f, 1.0f), material);

    auto* rigidbody = static_cast<RigidBodyComponent*>(entity->addComponent<RigidBodyComponent>());
    if (rigidbody) {
        rigidbody->setType("static");
    }

    auto* collision = static_cast<CollisionComponent*>(entity->addComponent<CollisionComponent>());
    if (collision) {
        collision->setType(type);
        if (type == "capsule") {
            collision->setHeight(2.0f);
        }
    }

    return entity;
}

void setSegmentMarker(Entity* marker, const Vector3& start, const Vector3& end, const float thickness)
{
    if (!marker) {
        return;
    }

    const Vector3 delta = end - start;
    const float length = delta.length();
    if (length <= 1e-5f) {
        marker->setLocalScale(Vector3(0.0f, 0.0f, 0.0f));
        return;
    }

    marker->setLocalPosition((start + end) * 0.5f);
    orientYAxisToDirection(marker, delta);
    marker->setLocalScale(Vector3(thickness, length, thickness));
}

int main()
{
    log::init();
    log::set_level_debug();

    const auto shutdown = []() {
        if (renderer) {
            SDL_DestroyRenderer(renderer);
            renderer = nullptr;
        }
        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
    };

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    SDL_Init(SDL_INIT_VIDEO);

    window = SDL_CreateWindow(
        "VisuTwin Physics Raycast Probe",
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        shutdown();
        return -1;
    }

    renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        shutdown();
        return -1;
    }
    SDL_SetRenderVSync(renderer, SDL_RENDERER_VSYNC_ADAPTIVE);

    auto* swapchain = static_cast<CA::MetalLayer*>(SDL_GetRenderMetalLayer(renderer));
    if (!swapchain) {
        shutdown();
        return -1;
    }

    auto device = createGraphicsDevice(GraphicsDeviceOptions{.swapChain = swapchain, .window = window});
    if (!device) {
        shutdown();
        return -1;
    }

    AppOptions createOptions;
    auto graphicsDevice = std::shared_ptr<GraphicsDevice>(std::move(device));
    createOptions.graphicsDevice = graphicsDevice;
    createOptions.registerComponentSystem<RenderComponentSystem>();
    createOptions.registerComponentSystem<CameraComponentSystem>();
    createOptions.registerComponentSystem<LightComponentSystem>();
    createOptions.registerComponentSystem<CollisionComponentSystem>();
    createOptions.registerComponentSystem<RigidBodyComponentSystem>();

    auto engine = std::make_shared<Engine>(window);
    engine->init(createOptions);
    engine->setCanvasFillMode(FillMode::FILLMODE_FILL_WINDOW);
    engine->setCanvasResolution(ResolutionMode::RESOLUTION_AUTO);
    engine->start();

    auto scene = engine->scene();
    scene->setAmbientLight(0.2f, 0.2f, 0.2f);

    auto green = std::make_shared<StandardMaterial>();
    green->setDiffuse(Color(0.0f, 1.0f, 0.0f, 1.0f));

    auto red = std::make_shared<StandardMaterial>();
    red->setDiffuse(Color(1.0f, 0.0f, 0.0f, 1.0f));

    auto white = std::make_shared<StandardMaterial>();
    white->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
    white->setEmissive(Color(1.0f, 1.0f, 1.0f, 1.0f));
    white->setEmissiveIntensity(8.0f);

    auto blue = std::make_shared<StandardMaterial>();
    blue->setDiffuse(Color(0.0f, 0.2f, 1.0f, 1.0f));
    blue->setEmissive(Color(0.0f, 0.2f, 1.0f, 1.0f));
    blue->setEmissiveIntensity(8.0f);

    auto* lightEntity = new Entity();
    lightEntity->setEngine(engine.get());
    auto* light = static_cast<LightComponent*>(lightEntity->addComponent<LightComponent>());
    if (light) {
        light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
        light->setIntensity(2.0f);
    }
    lightEntity->setLocalEulerAngles(45.0f, 30.0f, 0.0f);
    engine->root()->addChild(lightEntity);

    auto* cameraEntity = new Entity();
    cameraEntity->setEngine(engine.get());
    auto* camera = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
    if (camera && camera->camera()) {
        camera->camera()->setClearColor(Color(0.5f, 0.5f, 0.8f, 1.0f));
    }
    cameraEntity->setLocalPosition(5.0f, 0.0f, 15.0f);
    setLookAt(cameraEntity, Vector3(5.0f, 0.0f, 0.0f));
    engine->root()->addChild(cameraEntity);

    const std::vector<std::string> types = {"box", "capsule", "cone", "cylinder", "sphere"};
    std::vector<RenderComponent*> physicalRenders;
    physicalRenders.reserve(types.size() * 2u);

    for (size_t i = 0; i < types.size(); ++i) {
        auto* entity = createPhysicalShape(
            engine.get(),
            types[i],
            green.get(),
            Vector3(static_cast<float>(i) * 2.0f + 1.0f, 2.0f, 0.0f)
        );
        if (auto* render = entity->findComponent<RenderComponent>()) {
            physicalRenders.push_back(render);
        }
    }

    for (size_t i = 0; i < types.size(); ++i) {
        auto* entity = createPhysicalShape(
            engine.get(),
            types[i],
            green.get(),
            Vector3(static_cast<float>(i) * 2.0f + 1.0f, -2.0f, 0.0f)
        );
        if (auto* render = entity->findComponent<RenderComponent>()) {
            physicalRenders.push_back(render);
        }
    }

    auto* rayFirstMarker = createPrimitiveEntity(
        engine.get(),
        "cylinder",
        Vector3(0.0f, 2.0f, 0.0f),
        Vector3(0.03f, 1.0f, 0.03f),
        white.get()
    );
    auto* rayAllMarker = createPrimitiveEntity(
        engine.get(),
        "cylinder",
        Vector3(0.0f, -2.0f, 0.0f),
        Vector3(0.03f, 1.0f, 0.03f),
        white.get()
    );

    // DEVIATION: upstream uses app.drawLine for debug rays/normals; this native sample visualizes
    // them with thin cylinder render primitives until immediate line rendering is ported.
    std::vector<Entity*> normalMarkers;
    normalMarkers.reserve(16);
    for (int i = 0; i < 16; ++i) {
        auto* marker = createPrimitiveEntity(
            engine.get(),
            "cylinder",
            Vector3(0.0f, 0.0f, 0.0f),
            Vector3(0.0f, 0.0f, 0.0f),
            blue.get()
        );
        normalMarkers.push_back(marker);
    }

    auto* rigidbodySystem = dynamic_cast<RigidBodyComponentSystem*>(engine->systems()->getById("rigidbody"));
    if (!rigidbodySystem) {
        spdlog::error("RigidBodyComponentSystem not available");
        shutdown();
        return -1;
    }

    bool running = true;
    const uint64_t perfFreq = SDL_GetPerformanceFrequency();
    uint64_t prevCounter = SDL_GetPerformanceCounter();
    float time = 0.0f;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }

        const uint64_t nowCounter = SDL_GetPerformanceCounter();
        const float dt = static_cast<float>(static_cast<double>(nowCounter - prevCounter) / static_cast<double>(perfFreq));
        prevCounter = nowCounter;
        time += dt;

        for (auto* render : physicalRenders) {
            if (!render) {
                continue;
            }
            render->setMaterial(green.get());
        }

        for (auto* marker : normalMarkers) {
            marker->setLocalScale(Vector3(0.0f, 0.0f, 0.0f));
        }

        const float yFirst = 2.0f + 1.2f * std::sin(time);
        const Vector3 startFirst(0.0f, yFirst, 0.0f);
        const Vector3 endFirst(10.0f, yFirst, 0.0f);
        setSegmentMarker(rayFirstMarker, startFirst, endFirst, 0.03f);

        int normalMarkerCursor = 0;
        if (const auto hit = rigidbodySystem->raycastFirst(startFirst, endFirst); hit.has_value()) {
            if (auto* render = hit->entity ? hit->entity->findComponent<RenderComponent>() : nullptr) {
                render->setMaterial(red.get());
            }

            if (normalMarkerCursor < static_cast<int>(normalMarkers.size())) {
                const Vector3 normalEnd = hit->point + hit->normal * 0.8f;
                setSegmentMarker(normalMarkers[normalMarkerCursor], hit->point, normalEnd, 0.04f);
                normalMarkerCursor++;
            }
        }

        const float yAll = -2.0f + 1.2f * std::sin(time);
        const Vector3 startAll(0.0f, yAll, 0.0f);
        const Vector3 endAll(10.0f, yAll, 0.0f);
        setSegmentMarker(rayAllMarker, startAll, endAll, 0.03f);

        const auto allHits = rigidbodySystem->raycastAll(startAll, endAll);
        for (const auto& hit : allHits) {
            if (auto* render = hit.entity ? hit.entity->findComponent<RenderComponent>() : nullptr) {
                render->setMaterial(red.get());
            }

            if (normalMarkerCursor < static_cast<int>(normalMarkers.size())) {
                const Vector3 normalEnd = hit.point + hit.normal * 0.8f;
                setSegmentMarker(normalMarkers[normalMarkerCursor], hit.point, normalEnd, 0.04f);
                normalMarkerCursor++;
            }
        }

        engine->update(dt);
        engine->render();
    }

    shutdown();
    return 0;
}
