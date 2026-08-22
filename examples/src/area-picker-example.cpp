// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Area picker demo (parity with upstream graphics/area-picker): 300 random
// metallic primitives in a 30-unit box, lit only by the dim helipad env atlas.
// A quarter-resolution Picker renders mesh ids offscreen; every frame four
// screen areas are scanned — a tall yellow rectangle, a wide cyan strip, a
// tiny magenta square and a 1x1 red probe at the mouse — and the materials
// inside glow with the area's color (bloom makes them read as highlights).
// Pink outlines mark the areas on screen. Left-click places a green marker at
// the picked world point; right-click stops the auto-orbit and hands over to
// interactive camera controls.
//
// DEVIATIONS: upstream draws the outlines with app.drawLines and previews the
// picker's color/depth buffers with app.drawTexture — this engine has neither,
// so outlines are camera-parented unlit rods on the IMMEDIATE layer (ViewCube
// pattern) and the buffer previews are skipped.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "../cameraControls.h"
#include "../exampleApp.h"
#include "framework/assets/asset.h"
#include "framework/graphics/picker.h"
#include "platform/graphics/depthState.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

constexpr int WINDOW_WIDTH = 900;
constexpr int WINDOW_HEIGHT = 700;
constexpr float PICKER_SCALE = 0.25f;
constexpr float CAMERA_ORBIT_RADIUS = 40.0f;

// Screen-space rectangle outline: four thin unlit rods on the IMMEDIATE layer,
// parented to the camera and positioned in camera-local space at a fixed
// distance so they stay glued to the given window-pixel rectangle
// (upstream: camera.screenToWorld + app.drawLines).
class RectOutline
{
public:
    RectOutline(Engine* engine, Entity* cameraEntity, CameraComponent* cameraComponent)
        : _cameraEntity(cameraEntity), _cameraComponent(cameraComponent)
    {
        _material = std::make_shared<StandardMaterial>();
        _material->setUseLighting(false);
        _material->setDiffuse(Color(1.0f, 0.02f, 0.58f, 1.0f)); // upstream pink
        auto depthState = std::make_shared<DepthState>();
        depthState->setDepthTest(false);
        depthState->setDepthWrite(false);
        _material->setDepthState(depthState);

        for (auto& rod : _rods) {
            auto* entity = new Entity();
            entity->setEngine(engine);
            if (auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>())) {
                render->setLayers({LAYERID_IMMEDIATE});
                render->setMaterial(_material.get());
                render->setType("box");
                render->setCastShadows(false);
                render->setReceiveShadows(false);
            }
            cameraEntity->addChild(entity);
            rod = entity;
        }
    }

    // Rectangle in window-pixel coordinates (origin top-left).
    void update(const float x, const float y, const float w, const float h,
        const float windowW, const float windowH) const
    {
        if (!_cameraComponent || !_cameraComponent->camera() || windowW <= 0.0f || windowH <= 0.0f) {
            return;
        }
        constexpr float distance = 1.0f;   // camera-local plane the outline lives on
        const float fovRad = _cameraComponent->camera()->fov() * DEG_TO_RAD;
        const float halfH = distance * std::tan(fovRad * 0.5f);
        const float halfW = halfH * (windowW / windowH);

        // Window pixels -> camera-local units on the z = -distance plane.
        const auto toLocalX = [&](const float px) { return (px / windowW - 0.5f) * 2.0f * halfW; };
        const auto toLocalY = [&](const float py) { return (0.5f - py / windowH) * 2.0f * halfH; };

        const float x0 = toLocalX(x), x1 = toLocalX(x + w);
        const float y0 = toLocalY(y), y1 = toLocalY(y + h);
        const float t = 2.0f * (2.0f * halfH / windowH);  // ~2px rod thickness

        const auto place = [&](Entity* rod, const float cx, const float cy, const float sx, const float sy) {
            rod->setLocalPosition(cx, cy, -distance);
            rod->setLocalScale(std::max(sx, t), std::max(sy, t), t);
        };
        place(_rods[0], (x0 + x1) * 0.5f, y0, x1 - x0, t);  // top
        place(_rods[1], (x0 + x1) * 0.5f, y1, x1 - x0, t);  // bottom
        place(_rods[2], x0, (y0 + y1) * 0.5f, t, y0 - y1);  // left
        place(_rods[3], x1, (y0 + y1) * 0.5f, t, y0 - y1);  // right
    }

private:
    Entity* _cameraEntity = nullptr;
    CameraComponent* _cameraComponent = nullptr;
    std::shared_ptr<StandardMaterial> _material;
    std::array<Entity*, 4> _rods{};
};

class AreaPickerExample final: public ExampleApp
{
public:
    AreaPickerExample()
        : ExampleApp({.title = "Area Picker", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    bool create() override
    {
        // Upstream scene: dim skydome only — no analytical lights. Highlighted
        // objects glow via emissive + bloom.
        scene()->setSkyboxMip(2);
        scene()->setSkyboxIntensity(0.1f);

        _helipad = std::make_unique<Asset>(
            "helipad-env-atlas",
            AssetType::TEXTURE,
            assetPath("cubemaps/helipad-env-atlas.png"),
            AssetData{
                .type = TextureType::TEXTURETYPE_RGBP,
                .mipmaps = false
            }
        );
        const auto helipadResource = _helipad->resource();
        if (!helipadResource) {
            spdlog::error("Failed to load helipad texture");
            return false;
        }
        scene()->setEnvAtlas(std::get<Texture*>(*helipadResource));

        _cameraEntity = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _cameraComponent = _cameraEntity->findComponent<CameraComponent>();
        if (_cameraComponent && _cameraComponent->camera()) {
            _cameraComponent->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));
        }
        // Bloom (upstream CameraFrame bloom intensity 0.01).
        if (_cameraComponent) {
            auto rendering = _cameraComponent->rendering();
            rendering.bloomIntensity = 0.01f;
            _cameraComponent->setRendering(rendering);
        }

        _primitiveMaterials.reserve(320);
        std::mt19937 rng(0xC0FFEEu);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::uniform_real_distribution<float> scaleDistribution(1.0f, 2.0f);
        std::uniform_real_distribution<float> positionDistribution(-15.0f, 15.0f);
        std::uniform_int_distribution<int> shapeDistribution(0, 1);

        for (int i = 0; i < 300; ++i) {
            const bool useCylinder = shapeDistribution(rng) == 0;
            const char* primitiveType = useCylinder ? "cylinder" : "sphere";
            const Vector3 position(
                positionDistribution(rng),
                positionDistribution(rng),
                positionDistribution(rng)
            );
            const float scale = scaleDistribution(rng);

            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(unit(rng), unit(rng), unit(rng), 1.0f));
            material->setGloss(0.6f);
            material->setMetalness(0.4f);
            material->setUseMetalness(true);
            _primitiveMaterials.push_back(material);

            createPrimitive(primitiveType, material.get(), position, Vector3(scale, scale, scale));
        }

        // Green marker for the picked world point (upstream emissiveIntensity 100).
        // Lives on the UI layer so the WORLD-layer picker never sees it; hidden by
        // zero scale until the first successful pick.
        auto markerMaterial = std::make_shared<StandardMaterial>();
        markerMaterial->setEmissive(Color(0.0f, 1.0f, 0.0f, 1.0f));
        markerMaterial->setEmissiveIntensity(100.0f);
        _marker = createPrimitive("sphere", markerMaterial.get(),
            Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f), {LAYERID_UI});
        _primitiveMaterials.push_back(markerMaterial);

        _picker = std::make_unique<Picker>(engine(),
            static_cast<int>(WINDOW_WIDTH * PICKER_SCALE),
            static_cast<int>(WINDOW_HEIGHT * PICKER_SCALE), true);

        _mouseX = WINDOW_WIDTH / 2;
        _mouseY = WINDOW_HEIGHT / 2;

        // Outline rods for the four areas (all pink, like upstream drawRectangle).
        for (int i = 0; i < 4; ++i) {
            _outlines.push_back(std::make_unique<RectOutline>(engine(), _cameraEntity, _cameraComponent));
        }

        _highlightedMaterials.reserve(512);

        spdlog::info("Area picker: yellow/cyan/magenta areas + red mouse probe highlight picked objects.");
        spdlog::info("LMB = pick world point (green marker), RMB = take over camera, Esc = quit.");

        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            _mouseX = static_cast<int>(event.motion.x);
            _mouseY = static_cast<int>(event.motion.y);
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            _pendingWorldPick = std::make_pair(
                static_cast<int>(event.button.x * PICKER_SCALE),
                static_cast<int>(event.button.y * PICKER_SCALE)
            );
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
            if (_autoRotate) {
                _autoRotate = false;
                // CameraControls attaches at the camera's current pose (no jump).
                addOrbitControls(_cameraEntity, Vector3(0.0f, 0.0f, 0.0f));
                spdlog::info("Camera handed over to interactive controls");
            }
            return true;
        }
        return false;
    }

    void update(const float dt) override
    {
        const float deltaTime = std::clamp(dt, 0.0f, 0.1f);

        if (_autoRotate) {
            _time += deltaTime * 0.1f;
            _cameraEntity->setLocalPosition(Vector3(
                CAMERA_ORBIT_RADIUS * std::sin(_time),
                0.0f,
                CAMERA_ORBIT_RADIUS * std::cos(_time)
            ));
            _cameraEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        }

        int windowW = 0;
        int windowH = 0;
        SDL_GetWindowSize(window(), &windowW, &windowH);
        _picker->resize(
            std::max(1, static_cast<int>(windowW * PICKER_SCALE)),
            std::max(1, static_cast<int>(windowH * PICKER_SCALE))
        );

        _picker->prepare(_cameraComponent, scene().get(), _pickerLayers);

        for (auto* material : _highlightedMaterials) {
            if (!material) {
                continue;
            }
            material->setEmissive(Color(0.0f, 0.0f, 0.0f, 1.0f));
            material->setEmissiveIntensity(1.0f);
        }
        _highlightedMaterials.clear();

        std::unordered_set<StandardMaterial*> highlightedSet;

        const auto highlightSelection = [&](const std::vector<MeshInstance*>& selection, const Color& color) {
            for (auto* meshInstance : selection) {
                if (!meshInstance) {
                    continue;
                }
                auto* material = dynamic_cast<StandardMaterial*>(meshInstance->material());
                if (!material) {
                    continue;
                }
                material->setEmissive(color);
                material->setEmissiveIntensity(30.0f);
                if (highlightedSet.insert(material).second) {
                    _highlightedMaterials.push_back(material);
                }
            }
        };

        // Upstream areas: tall yellow rect, wide cyan strip, tiny magenta
        // square (proportional positions), and a 1x1 red probe at the mouse.
        struct PickArea
        {
            float x, y, w, h;
            Color color;
        };
        const float fw = static_cast<float>(windowW);
        const float fh = static_cast<float>(windowH);
        const std::array<PickArea, 4> areas = {{
            {fw * 0.3f, fh * 0.3f, 100.0f, 200.0f, Color(1.0f, 1.0f, 0.0f, 1.0f)},
            {fw * 0.6f, fh * 0.7f, 200.0f, 20.0f, Color(0.0f, 1.0f, 1.0f, 1.0f)},
            {fw * 0.8f, fh * 0.3f, 5.0f, 5.0f, Color(1.0f, 0.0f, 1.0f, 1.0f)},
            {static_cast<float>(_mouseX), static_cast<float>(_mouseY), 1.0f, 1.0f, Color(1.0f, 0.0f, 0.0f, 1.0f)},
        }};

        for (size_t i = 0; i < areas.size(); ++i) {
            const auto& area = areas[i];
            _outlines[i]->update(area.x, area.y, area.w, area.h, fw, fh);

            const auto selection = _picker->getSelection(
                static_cast<int>(area.x * PICKER_SCALE),
                static_cast<int>(area.y * PICKER_SCALE),
                std::max(1, static_cast<int>(area.w * PICKER_SCALE)),
                std::max(1, static_cast<int>(area.h * PICKER_SCALE))
            );
            highlightSelection(selection, area.color);
        }

        if (_pendingWorldPick.has_value()) {
            const auto [pickX, pickY] = *_pendingWorldPick;
            const auto worldPoint = _picker->getWorldPoint(pickX, pickY);
            if (worldPoint.has_value()) {
                _marker->setLocalPosition(*worldPoint);
                _marker->setLocalScale(0.2f, 0.2f, 0.2f);
            } else {
                _marker->setLocalScale(0.0f, 0.0f, 0.0f);
            }
            _pendingWorldPick.reset();
        }
    }

    void destroy() override
    {
        // Both borrow the engine's entities and render targets.
        _outlines.clear();
        _picker.reset();
    }

private:
    std::unique_ptr<Asset> _helipad;
    std::vector<std::shared_ptr<StandardMaterial>> _primitiveMaterials;
    std::vector<std::unique_ptr<RectOutline>> _outlines;
    std::unique_ptr<Picker> _picker;

    Entity* _cameraEntity = nullptr;
    CameraComponent* _cameraComponent = nullptr;
    Entity* _marker = nullptr;

    std::vector<int> _pickerLayers = {LAYERID_WORLD};
    std::vector<StandardMaterial*> _highlightedMaterials;
    std::optional<std::pair<int, int>> _pendingWorldPick;

    int _mouseX = 0;
    int _mouseY = 0;

    // Auto-orbit until the first right-click hands control to CameraControls.
    bool _autoRotate = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(AreaPickerExample)
