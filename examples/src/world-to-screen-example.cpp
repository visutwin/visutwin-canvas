// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/button/buttonComponentSystem.h"
#include "framework/components/collision/collisionComponent.h"
#include "framework/components/collision/collisionComponentSystem.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/rigidbody/rigidBodyComponent.h"
#include "framework/components/rigidbody/rigidBodyComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    struct PlayerUi
    {
        Entity* worldEntity = nullptr;
        std::shared_ptr<StandardMaterial> worldMaterial;
        float angleDeg = 0.0f;
        float speedDegPerSec = 0.0f;
        float radius = 1.0f;
        float health = 1.0f;

        Entity* panelEntity = nullptr;
        ElementComponent* panelElement = nullptr;
        Entity* nameEntity = nullptr;
        ElementComponent* nameElement = nullptr;
        Entity* healthEntity = nullptr;
        ElementComponent* healthElement = nullptr;
        ButtonComponent* nameButton = nullptr;
        Entity* panelVisual = nullptr;
        Entity* healthVisual = nullptr;
        std::shared_ptr<StandardMaterial> panelVisualMaterial;
        std::shared_ptr<StandardMaterial> healthVisualMaterial;
        bool visible = false;
        int visibleStreak = 0;
        int hiddenStreak = 0;
        Vector3 lastScreenPos = Vector3(0.0f, 0.0f, 0.0f);
        bool hasLastScreenPos = false;
    };

    Entity* createPrimitiveEntity(
        Engine* engine,
        const std::string& type,
        const Vector3& position,
        const Vector3& scale,
        StandardMaterial* material,
        const std::vector<int>& layers = {LAYERID_WORLD})
    {
        auto* entity = new Entity();
        entity->setEngine(engine);
        entity->setLocalPosition(position);
        entity->setLocalScale(scale);

        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        if (render) {
            render->setType(type);
            render->setMaterial(material);
            render->setLayers(layers);
        }

        engine->root()->addChild(entity);
        return entity;
    }

    bool worldToScreenSpace(
        const Vector3& worldPosition,
        CameraComponent* cameraComponent,
        Entity* cameraEntity,
        ScreenComponent* screenComponent,
        Vector3& outScreen)
    {
        if (!cameraComponent || !cameraComponent->camera() || !cameraEntity || !screenComponent) {
            return false;
        }

        const Matrix4 view = cameraEntity->worldTransform().inverse();
        const Matrix4 proj = cameraComponent->camera()->projectionMatrix();
        const Vector3 viewPos = view.transformPoint(worldPosition);

        if (viewPos.getZ() >= 0.0f) {
            return false;
        }

        const Vector4 clip = proj * Vector4(viewPos.getX(), viewPos.getY(), viewPos.getZ(), 1.0f);
        if (std::abs(clip.getW()) < 1e-6f) {
            return false;
        }

        const float ndcX = clip.getX() / clip.getW();
        const float ndcY = clip.getY() / clip.getW();

        const Vector2 screenRes = screenComponent->resolution();
        const float sxPx = (ndcX * 0.5f + 0.5f) * screenRes.x;
        const float syPx = (1.0f - (ndcY * 0.5f + 0.5f)) * screenRes.y;

        const float scale = std::max(screenComponent->scale(), 1e-6f);
        outScreen = Vector3(sxPx / scale, syPx / scale, (-viewPos.getZ()) / scale);
        return true;
    }

    PlayerUi createPlayer(Engine* engine, Entity* screenEntity, int id, float startAngle, float speed, float radius)
    {
        PlayerUi player;
        player.angleDeg = startAngle;
        player.speedDegPerSec = speed;
        player.radius = radius;

        player.worldMaterial = std::make_shared<StandardMaterial>();
        player.worldMaterial->setDiffuse(Color(0.85f, 0.85f, 0.9f, 1.0f));
        player.worldEntity = createPrimitiveEntity(
            engine,
            "capsule",
            Vector3(0.0f, 0.5f, 0.0f),
            Vector3(0.5f, 0.5f, 0.5f),
            player.worldMaterial.get()
        );

        if (auto* body = static_cast<RigidBodyComponent*>(player.worldEntity->addComponent<RigidBodyComponent>())) {
            body->setType("static");
        }
        if (auto* col = static_cast<CollisionComponent*>(player.worldEntity->addComponent<CollisionComponent>())) {
            col->setType("capsule");
            col->setRadius(0.35f);
            col->setHeight(1.0f);
        }

        player.panelEntity = new Entity();
        player.panelEntity->setEngine(engine);
        player.panelElement = static_cast<ElementComponent*>(player.panelEntity->addComponent<ElementComponent>());
        if (player.panelElement) {
            player.panelElement->setType(ElementType::Image);
            player.panelElement->setPivot(Vector2(0.5f, 0.0f));
            player.panelElement->setAnchor(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            player.panelElement->setWidth(150.0f);
            player.panelElement->setHeight(50.0f);
            player.panelElement->setColor(Color(0.0f, 0.0f, 0.0f, 1.0f));
            player.panelElement->setOpacity(0.35f);
        }
        screenEntity->addChild(player.panelEntity);

        player.nameEntity = new Entity();
        player.nameEntity->setEngine(engine);
        player.nameEntity->setLocalPosition(0.0f, 4.0f, 0.0f);
        player.nameElement = static_cast<ElementComponent*>(player.nameEntity->addComponent<ElementComponent>());
        if (player.nameElement) {
            player.nameElement->setType(ElementType::Text);
            player.nameElement->setPivot(Vector2(0.5f, 0.0f));
            player.nameElement->setWidth(130.0f);
            player.nameElement->setHeight(20.0f);
            player.nameElement->setFontSize(22);
            player.nameElement->setText("Player " + std::to_string(id));
            player.nameElement->setColor(Color(1.0f, 1.0f, 1.0f, 1.0f));
            player.nameElement->setHorizontalAlign(ElementHorizontalAlign::Center);
            player.nameElement->setWrapLines(false);
            player.nameElement->setUseInput(true);
        }
        player.nameButton = static_cast<ButtonComponent*>(player.nameEntity->addComponent<ButtonComponent>());
        if (player.nameButton) {
            player.nameButton->setImageEntity(player.nameEntity);
        }
        player.panelEntity->addChild(player.nameEntity);

        player.healthEntity = new Entity();
        player.healthEntity->setEngine(engine);
        player.healthEntity->setLocalPosition(0.0f, 32.0f, 0.0f);
        player.healthElement = static_cast<ElementComponent*>(player.healthEntity->addComponent<ElementComponent>());
        if (player.healthElement) {
            player.healthElement->setType(ElementType::Image);
            player.healthElement->setPivot(Vector2(0.5f, 0.0f));
            player.healthElement->setWidth(130.0f);
            player.healthElement->setHeight(10.0f);
            player.healthElement->setColor(Color(0.2f, 0.6f, 0.2f, 1.0f));
            player.healthElement->setOpacity(1.0f);
        }
        player.panelEntity->addChild(player.healthEntity);

        // Visual representation rendered by engine on UI layer (stable single-present path).
        player.panelVisualMaterial = std::make_shared<StandardMaterial>();
        player.panelVisualMaterial->setUseLighting(false);
        player.panelVisualMaterial->setUseSkybox(false);
        player.panelVisualMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        player.panelVisualMaterial->setEmissive(Color(0.0f, 0.0f, 0.0f, 1.0f));
        player.panelVisualMaterial->setOpacity(0.35f);
        player.panelVisualMaterial->setTransparent(true);
        player.panelVisualMaterial->setCullMode(CullMode::CULLFACE_NONE);
        player.panelVisual = createPrimitiveEntity(
            engine,
            "box",
            Vector3(0.0f, 0.0f, 0.0f),
            Vector3(150.0f, 50.0f, 0.2f),
            player.panelVisualMaterial.get(),
            {LAYERID_UI}
        );

        player.healthVisualMaterial = std::make_shared<StandardMaterial>();
        player.healthVisualMaterial->setUseLighting(false);
        player.healthVisualMaterial->setUseSkybox(false);
        player.healthVisualMaterial->setDiffuse(Color(0.2f, 0.6f, 0.2f, 1.0f));
        player.healthVisualMaterial->setEmissive(Color(0.2f, 0.6f, 0.2f, 1.0f));
        player.healthVisualMaterial->setTransparent(true);
        player.healthVisualMaterial->setCullMode(CullMode::CULLFACE_NONE);
        player.healthVisual = createPrimitiveEntity(
            engine,
            "box",
            Vector3(0.0f, -12.0f, 0.0f),
            Vector3(130.0f, 10.0f, 0.2f),
            player.healthVisualMaterial.get(),
            {LAYERID_UI}
        );

        return player;
    }

    bool isOccluded(const Vector3& cameraPos, const Vector3& targetPos, Entity* target, const RigidBodyComponentSystem* rb)
    {
        if (!target || !rb) {
            return false;
        }
        const auto hits = rb->raycastAll(cameraPos, targetPos);
        if (hits.empty()) {
            return false;
        }
        const float targetDistSq = (targetPos - cameraPos).lengthSquared();
        for (const auto& hit : hits) {
            if (!hit.entity) {
                continue;
            }
            if (hit.entity == target || hit.entity->isDescendantOf(target)) {
                continue;
            }
            const float hitDistSq = (hit.point - cameraPos).lengthSquared();
            // Treat as occluder only if it is clearly before target point.
            if (hitDistSq + 1e-4f < targetDistSq) {
                return true;
            }
        }
        return false;
    }
}

class WorldToScreenExample final: public ExampleApp
{
public:
    WorldToScreenExample()
        : ExampleApp({.title = "Screen Overlay Anchors (World-To-Screen)",
                      .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<CollisionComponentSystem>();
        options.registerComponentSystem<RigidBodyComponentSystem>();
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        options.registerComponentSystem<ButtonComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        scene()->setAmbientLight(0.3f, 0.3f, 0.32f);

        _checkerboard = std::make_unique<Asset>(
            "checkerboard", AssetType::TEXTURE, assetPath("textures/checkboard.png"));
        _courierFont = std::make_unique<Asset>(
            "courier-font", AssetType::FONT, assetPath("fonts/courier.json"));

        _groundMaterial = std::make_shared<StandardMaterial>();
        if (const auto checkerResource = _checkerboard->resource()) {
            _groundMaterial->setDiffuseMap(std::get<Texture*>(*checkerResource));
        } else {
            _groundMaterial->setDiffuse(Color(0.75f, 0.75f, 0.75f, 1.0f));
        }

        auto* ground = createPrimitive("box", _groundMaterial.get(), Vector3(0.0f, -0.5f, 0.0f),
            Vector3(50.0f, 1.0f, 50.0f), {LAYERID_WORLD});
        if (auto* body = static_cast<RigidBodyComponent*>(ground->addComponent<RigidBodyComponent>())) {
            body->setType("static");
        }
        if (auto* col = static_cast<CollisionComponent*>(ground->addComponent<CollisionComponent>())) {
            col->setType("box");
            col->setHalfExtents(Vector3(25.0f, 0.5f, 25.0f));
        }

        _occluderMaterial = std::make_shared<StandardMaterial>();
        _occluderMaterial->setDiffuse(Color(0.55f, 0.58f, 0.64f, 1.0f));
        auto* occluder = createPrimitive("box", _occluderMaterial.get(), Vector3(0.0f, 1.25f, 0.0f),
            Vector3(1.5f, 2.5f, 0.6f), {LAYERID_WORLD});
        if (auto* body = static_cast<RigidBodyComponent*>(occluder->addComponent<RigidBodyComponent>())) {
            body->setType("static");
        }
        if (auto* col = static_cast<CollisionComponent*>(occluder->addComponent<CollisionComponent>())) {
            col->setType("box");
            col->setHalfExtents(Vector3(0.75f, 1.25f, 0.3f));
        }

        auto* light = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowResolution(2048);
            lightComp->setShadowDistance(16.0f);
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowNormalBias(0.05f);
        }

        _cameraEntity = createCamera(Vector3(0.0f, 3.5f, 7.0f), Vector3(-30.0f, 0.0f, 0.0f));
        _cameraComponent = _cameraEntity->findComponent<CameraComponent>();
        if (_cameraComponent && _cameraComponent->camera()) {
            _cameraComponent->camera()->setClearColor(
                Color(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f));
        }
        _cameraComponent->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX});

        // Orthographic UI camera rendering only the UI layer.
        auto* uiCameraEntity = createCamera(Vector3(0.0f, 0.0f, 10.0f));
        _uiCamera = uiCameraEntity->findComponent<CameraComponent>();
        if (_uiCamera && _uiCamera->camera()) {
            _uiCamera->camera()->setProjection(ProjectionType::Orthographic);
            _uiCamera->camera()->setOrthoHeight(static_cast<float>(WINDOW_HEIGHT) * 0.5f);
            _uiCamera->camera()->setClearColorBuffer(false);
            // Render UI over world by clearing depth before UI pass.
            _uiCamera->camera()->setClearDepthBuffer(true);
            _uiCamera->camera()->setClearStencilBuffer(true);
            _uiCamera->setLayers({LAYERID_UI});
        }

        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screenComponent = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        if (_screenComponent) {
            _screenComponent->setReferenceResolution(Vector2(1280.0f, 720.0f));
            _screenComponent->setScreenSpace(true);
        }
        root()->addChild(screenEntity);

        _players.reserve(3);
        _players.push_back(createPlayer(engine(), screenEntity, 1, 135.0f, 30.0f, 1.5f));
        _players.push_back(createPlayer(engine(), screenEntity, 2, 65.0f, -18.0f, 1.0f));
        _players.push_back(createPlayer(engine(), screenEntity, 3, 0.0f, 15.0f, 2.5f));

        FontResource* fontResource = nullptr;
        if (const auto fontRes = _courierFont->resource();
            fontRes.has_value() && std::holds_alternative<FontResource*>(*fontRes)) {
            fontResource = std::get<FontResource*>(*fontRes);
        }
        if (fontResource) {
            for (auto& player : _players) {
                if (player.nameElement) {
                    player.nameElement->setFontResource(fontResource);
                }
            }
        } else {
            spdlog::warn("Courier bitmap font was not loaded. Text labels will remain disabled.");
        }

        std::uniform_real_distribution<float> unit(0.15f, 1.0f);
        for (auto& player : _players) {
            if (!player.nameButton) {
                continue;
            }
            player.nameButton->on("click",
                [nameEl = player.nameElement, mat = player.worldMaterial, this, unit]() mutable {
                    const Color color(unit(_rng), unit(_rng), unit(_rng), 1.0f);
                    if (nameEl) {
                        nameEl->setColor(color);
                    }
                    if (mat) {
                        mat->setDiffuse(color);
                    }
                });
        }

        _rigidbodySystem = dynamic_cast<RigidBodyComponentSystem*>(engine()->systems()->getById("rigidbody"));
        spdlog::info("World-To-Screen controls: ESC quit, left-click player name recolor, O toggle occlusion ({})",
            _occlusionEnabled ? "on" : "off");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_O) {
            _occlusionEnabled = !_occlusionEnabled;
            spdlog::info("Occlusion gating: {}", _occlusionEnabled ? "enabled" : "disabled");
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            _elementInput->handleMouseButtonDown(event.button.x, event.button.y);
            return true;
        }
        return false;
    }

    void update(const float dt) override
    {
        int windowW = 1;
        int windowH = 1;
        SDL_GetWindowSize(window(), &windowW, &windowH);
        _uiWidth = static_cast<float>(windowW);
        _uiHeight = static_cast<float>(windowH);
        if (_screenComponent) {
            _screenComponent->updateScaleFromWindow(windowW, windowH);
            const float scale = std::max(_screenComponent->scale(), 1e-6f);
            _uiWidth = _screenComponent->resolution().x / scale;
            _uiHeight = _screenComponent->resolution().y / scale;
        }
        if (_uiCamera && _uiCamera->camera()) {
            _uiCamera->camera()->setOrthoHeight(_uiHeight * 0.5f);
        }

        for (auto& player : _players) {
            player.angleDeg += dt * player.speedDegPerSec;
            if (player.angleDeg > 360.0f) {
                player.angleDeg -= 360.0f;
            } else if (player.angleDeg < -360.0f) {
                player.angleDeg += 360.0f;
            }

            const float rad = player.angleDeg * DEG_TO_RAD;
            const float x = player.radius * std::sin(rad);
            const float z = player.radius * std::cos(rad);
            player.worldEntity->setLocalPosition(x, 0.5f, z);
            player.worldEntity->setLocalEulerAngles(0.0f, player.angleDeg + 90.0f, 0.0f);

            player.health = 0.75f;
        }
    }

    // Runs after Engine::update so the world transforms the overlay anchors to are
    // the ones this frame will render.
    void preRender() override
    {
        for (auto& player : _players) {
            const Vector3 base = player.worldEntity->position();
            const Vector3 headWorld(base.getX(), base.getY() + 0.6f, base.getZ());

            Vector3 screenPos;
            const bool inFront = worldToScreenSpace(headWorld, _cameraComponent, _cameraEntity,
                _screenComponent, screenPos);
            const bool inBounds = inFront &&
                screenPos.getX() >= 16.0f && screenPos.getX() <= _uiWidth - 16.0f &&
                screenPos.getY() >= 16.0f && screenPos.getY() <= _uiHeight - 16.0f;
            const bool occluded = _occlusionEnabled && inBounds &&
                isOccluded(_cameraEntity->position(), headWorld, player.worldEntity, _rigidbodySystem);
            const bool desiredVisible = inBounds && screenPos.getZ() > 0.0f && !occluded;

            player.panelEntity->setEnabled(desiredVisible);
            if (player.nameEntity) {
                player.nameEntity->setEnabled(desiredVisible);
            }
            if (player.healthEntity) {
                player.healthEntity->setEnabled(desiredVisible);
            }
            if (player.panelVisual) {
                player.panelVisual->setEnabled(desiredVisible);
            }
            if (player.healthVisual) {
                player.healthVisual->setEnabled(desiredVisible);
            }
            if (desiredVisible) {
                player.panelEntity->setLocalPosition(screenPos.getX(), screenPos.getY(), 0.0f);

                const float worldX = screenPos.getX() - _uiWidth * 0.5f;
                const float worldY = _uiHeight * 0.5f - screenPos.getY();
                if (player.panelVisual) {
                    player.panelVisual->setLocalPosition(worldX, worldY, 0.0f);
                    player.panelVisual->setLocalScale(150.0f, 50.0f, 0.2f);
                }

                if (player.healthElement) {
                    const float healthW = 130.0f * std::clamp(player.health, 0.0f, 1.0f);
                    player.healthElement->setWidth(healthW);
                    if (player.healthVisual) {
                        player.healthVisual->setLocalScale(healthW, 10.0f, 1.0f);
                        player.healthVisual->setLocalPosition(worldX - 65.0f + healthW * 0.5f, worldY - 12.0f, 0.0f);
                    }
                }
            } else {
                // Prevent stale overlays in case enable-state propagation is delayed.
                player.panelEntity->setLocalPosition(-10000.0f, -10000.0f, 0.0f);
                if (player.panelVisual) {
                    player.panelVisual->setLocalPosition(-10000.0f, -10000.0f, 0.0f);
                }
                if (player.healthVisual) {
                    player.healthVisual->setLocalPosition(-10000.0f, -10000.0f, 0.0f);
                }
            }
        }

        _elementInput->syncTextElements();
    }

private:
    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _checkerboard;
    std::unique_ptr<Asset> _courierFont;

    std::shared_ptr<StandardMaterial> _groundMaterial;
    std::shared_ptr<StandardMaterial> _occluderMaterial;

    std::vector<PlayerUi> _players;
    Entity* _cameraEntity = nullptr;
    CameraComponent* _cameraComponent = nullptr;
    CameraComponent* _uiCamera = nullptr;
    ScreenComponent* _screenComponent = nullptr;
    RigidBodyComponentSystem* _rigidbodySystem = nullptr;

    std::mt19937 _rng{1337u};
    bool _occlusionEnabled = false;
    float _uiWidth = static_cast<float>(WINDOW_WIDTH);
    float _uiHeight = static_cast<float>(WINDOW_HEIGHT);
};

VISUTWIN_EXAMPLE_MAIN(WorldToScreenExample)
