// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Port of upstream user-interface/world-to-screen.
//
// Three capsule "players" circle the origin at their own radius and speed on a
// 50x50 checkerboard ground (tiled 50x) under a shadow-casting directional
// light, seen from a camera pitched down 30 degrees at distance 7. Each player
// carries a screen-space panel that follows its head: a faint 150x50 backing,
// a "Player N" name in its top 60% and a green health bar in its bottom 40%.
// Clicking a name recolours it and its player with a random colour.
//
// DEVIATIONS:
// - image elements are not rendered by the element system, so the panel backing
//   and the health bar are unlit boxes on LAYERID_UI with the elements' colour,
//   opacity and rectangle. UI is drawn by a separate orthographic camera.
// - the element system has no anchors or margins; the name and health bar are
//   placed in UI pixels (top-left origin) inside the panel rectangle that
//   upstream's anchors describe.
// - there is no CameraComponent::worldToScreen; the projection is done here.
// - each player has its own white StandardMaterial and a click sets its diffuse,
//   where upstream overrides material_diffuse on the mesh instance of a shared
//   default material. Both store the colour in gamma space and decode it.
//
#include <algorithm>
#include <cmath>
#include <memory>
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
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "framework/input/elementInput.h"
#include "platform/graphics/blendState.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 1280;
constexpr int WINDOW_HEIGHT = 720;

namespace
{
    constexpr float PANEL_WIDTH = 150.0f;
    constexpr float PANEL_HEIGHT = 50.0f;

    struct Player
    {
        Entity* entity = nullptr;
        std::shared_ptr<StandardMaterial> material;
        float angle = 0.0f;
        float speed = 0.0f;
        float radius = 1.0f;

        Entity* playerInfo = nullptr;
        ElementComponent* name = nullptr;
        ButtonComponent* button = nullptr;

        Entity* panelVisual = nullptr;
        Entity* healthVisual = nullptr;
        std::shared_ptr<StandardMaterial> panelMaterial;
        std::shared_ptr<StandardMaterial> healthMaterial;
    };

    std::shared_ptr<StandardMaterial> makeUiMaterial(const Color& color, const float opacity)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setUseLighting(false);
        material->setUseSkybox(false);
        material->setDiffuse(color);
        material->setEmissive(color);
        material->setOpacity(opacity);
        material->setTransparent(true);
        material->setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
        material->setCullMode(CullMode::CULLFACE_NONE);
        return material;
    }

    /// Converts a coordinate in world space into the screen's space, in UI pixels
    /// with a top-left origin. z is the depth; <= 0 means behind the camera.
    Vector3 worldToScreenSpace(const Vector3& worldPosition, CameraComponent* camera, ScreenComponent* screen)
    {
        const Matrix4 view = camera->entity()->worldTransform().inverse();
        const Vector3 viewPos = view.transformPoint(worldPosition);
        const Vector4 clip = camera->camera()->projectionMatrix() *
            Vector4(viewPos.getX(), viewPos.getY(), viewPos.getZ(), 1.0f);
        if (viewPos.getZ() >= 0.0f || std::abs(clip.getW()) < 1e-6f) {
            return Vector3(0.0f, 0.0f, -1.0f);
        }

        const Vector2 resolution = screen->resolution();
        const float x = (clip.getX() / clip.getW() * 0.5f + 0.5f) * resolution.x;
        const float y = (0.5f - clip.getY() / clip.getW() * 0.5f) * resolution.y;
        const float scale = std::max(screen->scale(), 1e-6f);
        return Vector3(x / scale, y / scale, -viewPos.getZ() / scale);
    }
}

class WorldToScreenExample final: public ExampleApp
{
public:
    WorldToScreenExample()
        : ExampleApp({.title = "World To Screen", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    void configure(AppOptions& options) override
    {
        options.registerComponentSystem<ScreenComponentSystem>();
        options.registerComponentSystem<ButtonComponentSystem>();
        options.registerComponentSystem<ElementComponentSystem>();
        _elementInput = std::make_shared<ElementInput>();
        options.elementInput = _elementInput;
    }

    bool create() override
    {
        _checkboard = std::make_unique<Asset>(
            "checkboard", AssetType::TEXTURE, assetPath("textures/checkboard.png"),
            AssetData{.mipmaps = true});
        _font = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/courier.json"));

        FontResource* font = nullptr;
        if (const auto fontRes = _font->resource();
            fontRes.has_value() && std::holds_alternative<FontResource*>(*fontRes)) {
            font = std::get<FontResource*>(*fontRes);
        }
        const auto checkboardRes = _checkboard->resource();
        if (!font || !checkboardRes) {
            spdlog::error("Failed to load the checkboard texture or the courier font");
            return false;
        }

        // Create an Entity with a camera component: rotateLocal(-30, 0, 0), then
        // translateLocal(0, 0, 7) along the rotated axis.
        const float pitch = -30.0f * DEG_TO_RAD;
        _camera = createCamera(Vector3(0.0f, -7.0f * std::sin(pitch), 7.0f * std::cos(pitch)),
            Vector3(-30.0f, 0.0f, 0.0f))->findComponent<CameraComponent>();
        _camera->camera()->setClearColor(Color(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f));
        _camera->setLayers({LAYERID_WORLD, LAYERID_DEPTH, LAYERID_SKYBOX});

        // Orthographic camera for the UI layer, drawn over the main camera.
        auto* uiCameraEntity = createCamera(Vector3(0.0f, 0.0f, 10.0f));
        _uiCamera = uiCameraEntity->findComponent<CameraComponent>();
        _uiCamera->camera()->setProjection(ProjectionType::Orthographic);
        _uiCamera->camera()->setOrthoHeight(static_cast<float>(WINDOW_HEIGHT) * 0.5f);
        _uiCamera->camera()->setClearColorBuffer(false);
        _uiCamera->camera()->setClearDepthBuffer(true);
        _uiCamera->camera()->setClearStencilBuffer(true);
        _uiCamera->setLayers({LAYERID_UI});

        // Create an Entity for the ground
        _groundMaterial = std::make_shared<StandardMaterial>();
        _groundMaterial->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        _groundMaterial->setDiffuseMap(std::get<Texture*>(*checkboardRes));
        _groundMaterial->setDiffuseMapTiling(Vector2(50.0f, 50.0f));
        createPrimitive("box", _groundMaterial.get(), Vector3(0.0f, -0.5f, 0.0f), Vector3(50.0f, 1.0f, 50.0f));

        // Create an Entity with a light component
        auto* light = createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f),
            Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComp = light->findComponent<LightComponent>()) {
            lightComp->setShadowBias(0.2f);
            lightComp->setShadowDistance(16.0f);
            lightComp->setShadowNormalBias(0.05f);
            lightComp->setShadowResolution(2048);
        }

        // Create a 2D screen
        auto* screenEntity = new Entity();
        screenEntity->setEngine(engine());
        _screen = static_cast<ScreenComponent*>(screenEntity->addComponent<ScreenComponent>());
        _screen->setReferenceResolution(Vector2(1280.0f, 720.0f));
        _screen->setScreenSpace(true);
        root()->addChild(screenEntity);

        _players.reserve(3);
        createPlayer(screenEntity, font, 1, 135.0f, 30.0f, 1.5f);
        createPlayer(screenEntity, font, 2, 65.0f, -18.0f, 1.0f);
        createPlayer(screenEntity, font, 3, 0.0f, 15.0f, 2.5f);

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            return _elementInput->handleMouseButtonDown(event.button.x, event.button.y);
        }
        return false;
    }

    void update(const float dt) override
    {
        int windowW = 1;
        int windowH = 1;
        SDL_GetWindowSize(window(), &windowW, &windowH);
        _screen->updateScaleFromWindow(windowW, windowH);
        const float scale = std::max(_screen->scale(), 1e-6f);
        _uiWidth = _screen->resolution().x / scale;
        _uiHeight = _screen->resolution().y / scale;
        _uiCamera->camera()->setOrthoHeight(_uiHeight * 0.5f);

        // Update the player position every frame with some mock logic
        for (auto& player : _players) {
            player.angle += dt * player.speed;
            if (player.angle > 360.0f) {
                player.angle -= 360.0f;
            }
            player.entity->setLocalPosition(
                player.radius * std::sin(player.angle * DEG_TO_RAD),
                0.5f,
                player.radius * std::cos(player.angle * DEG_TO_RAD));
            player.entity->setLocalEulerAngles(0.0f, player.angle + 90.0f, 0.0f);
        }
    }

    // After Engine::update, so the overlay follows the transforms this frame renders.
    void preRender() override
    {
        for (auto& player : _players) {
            // Slightly above the player's head
            Vector3 worldPosition = player.entity->position();
            worldPosition = Vector3(worldPosition.getX(), worldPosition.getY() + 0.6f, worldPosition.getZ());

            const Vector3 screenPosition = worldToScreenSpace(worldPosition, _camera, _screen);
            const bool inFront = screenPosition.getZ() > 0.0f;

            player.playerInfo->setEnabled(inFront);
            player.panelVisual->setEnabled(inFront);
            player.healthVisual->setEnabled(inFront);
            if (!inFront) {
                continue;
            }

            // The panel's pivot is its bottom centre, at the projected point.
            const float sx = screenPosition.getX();
            const float sy = screenPosition.getY();
            player.playerInfo->setLocalPosition(sx, sy, 0.0f);

            // UI pixels (y down) to the ortho UI camera's centred, y-up space.
            const float cx = sx - _uiWidth * 0.5f;
            const float bottom = _uiHeight * 0.5f - sy;
            player.panelVisual->setLocalPosition(cx, bottom + PANEL_HEIGHT * 0.5f, 0.0f);
            player.healthVisual->setLocalPosition(cx, bottom + PANEL_HEIGHT * 0.2f, 1.0f);
        }

        _elementInput->syncTextElements();
    }

private:
    void createPlayer(Entity* screenEntity, FontResource* font, const int id,
        const float startingAngle, const float speed, const float radius)
    {
        auto& player = _players.emplace_back();
        player.angle = startingAngle;
        player.speed = speed;
        player.radius = radius;

        // Create a capsule entity to represent a player in the 3d world
        player.material = std::make_shared<StandardMaterial>();
        player.entity = createPrimitive("capsule", player.material.get(),
            Vector3(0.0f, 0.5f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));

        // The panel that hovers over the player's head: pivot (0.5, 0), 150x50.
        player.playerInfo = new Entity();
        player.playerInfo->setEngine(engine());
        if (auto* info = static_cast<ElementComponent*>(player.playerInfo->addComponent<ElementComponent>())) {
            info->setType(ElementType::Image);
            info->setPivot(Vector2(0.5f, 0.0f));
            info->setAnchor(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            info->setWidth(PANEL_WIDTH);
            info->setHeight(PANEL_HEIGHT);
            info->setOpacity(0.05f);
        }
        screenEntity->addChild(player.playerInfo);

        // Name: anchor (0, 0.4, 1, 1), so the top 60% of the panel, centred in it.
        // The text mesh starts its first line at the TOP of the element box (there is
        // no vertical alignment), so the box is one line high, centred in that region.
        constexpr int nameFontSize = 20;
        const float nameRegionHeight = PANEL_HEIGHT * 0.6f;
        auto* nameEntity = new Entity();
        nameEntity->setEngine(engine());
        nameEntity->setLocalPosition(0.0f, -(PANEL_HEIGHT - nameRegionHeight * 0.5f), 0.0f);
        player.name = static_cast<ElementComponent*>(nameEntity->addComponent<ElementComponent>());
        player.name->setType(ElementType::Text);
        player.name->setPivot(Vector2(0.5f, 0.5f));
        player.name->setWidth(PANEL_WIDTH);
        player.name->setHeight(static_cast<float>(nameFontSize));
        player.name->setFontResource(font);
        player.name->setFontSize(nameFontSize);
        player.name->setText("Player " + std::to_string(id));
        player.name->setHorizontalAlign(ElementHorizontalAlign::Center);
        player.name->setUseInput(true);
        player.button = static_cast<ButtonComponent*>(nameEntity->addComponent<ButtonComponent>());
        player.button->setImageEntity(nameEntity);
        player.playerInfo->addChild(nameEntity);

        // Health bar: anchor (0, 0, 1, 0.4), so the bottom 40% of the panel.
        auto* healthBar = new Entity();
        healthBar->setEngine(engine());
        if (auto* health = static_cast<ElementComponent*>(healthBar->addComponent<ElementComponent>())) {
            health->setType(ElementType::Image);
            health->setPivot(Vector2(0.5f, 0.0f));
            health->setWidth(PANEL_WIDTH);
            health->setHeight(PANEL_HEIGHT * 0.4f);
            health->setColor(Color(0.2f, 0.6f, 0.2f, 1.0f));
            health->setOpacity(1.0f);
        }
        player.playerInfo->addChild(healthBar);

        // The two image elements, drawn as boxes on the UI layer.
        player.panelMaterial = makeUiMaterial(Color(1.0f, 1.0f, 1.0f, 1.0f), 0.05f);
        player.panelVisual = createPrimitive("box", player.panelMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(PANEL_WIDTH, PANEL_HEIGHT, 0.2f), {LAYERID_UI});
        player.healthMaterial = makeUiMaterial(Color(0.2f, 0.6f, 0.2f, 1.0f), 1.0f);
        player.healthVisual = createPrimitive("box", player.healthMaterial.get(), Vector3(0.0f, 0.0f, 0.0f),
            Vector3(PANEL_WIDTH, PANEL_HEIGHT * 0.4f, 0.2f), {LAYERID_UI});

        player.button->on("click", [this, name = player.name, material = player.material]() {
            std::uniform_real_distribution<float> unit(0.0f, 1.0f);
            const Color color(unit(_rng), unit(_rng), unit(_rng), 1.0f);
            name->setColor(color);
            material->setDiffuse(color);
        });
    }

    std::shared_ptr<ElementInput> _elementInput;
    std::unique_ptr<Asset> _checkboard;
    std::unique_ptr<Asset> _font;
    std::shared_ptr<StandardMaterial> _groundMaterial;

    std::vector<Player> _players;
    CameraComponent* _camera = nullptr;
    CameraComponent* _uiCamera = nullptr;
    ScreenComponent* _screen = nullptr;

    std::mt19937 _rng{std::random_device{}()};
    float _uiWidth = static_cast<float>(WINDOW_WIDTH);
    float _uiHeight = static_cast<float>(WINDOW_HEIGHT);
};

VISUTWIN_EXAMPLE_MAIN(WorldToScreenExample)
