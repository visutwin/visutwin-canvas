// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Port of upstream graphics/area-picker.
//
// 300 clay-coloured shapes (box, sphere, cylinder, capsule, cone, torus; gloss 0.45) are scattered
// in a ball of radius 16, at random sizes and rotations. A quarter-resolution Picker renders their
// ids (and depth) offscreen every frame, and four screen areas are scanned: a tall amber rectangle,
// a wide cyan strip, a small magenta square and a 1 x 1 red probe at the mouse. The objects inside
// each glow with the area's colour (their emissive, bloomed), and each area is outlined in its
// colour. Left-click places a green marker at the picked world point (the marker opts out of
// picking); right-click stops the auto-orbit and hands the camera to orbit controls. The lights
// ride with the camera: a warm key light with soft shadows fitted to the ball, and a cool rim light
// from behind. The studio environment (intensity 0.2) gives reflections; the camera (neutral tone
// mapping, bloom, vignette) renders no skybox against a dark clear colour. The picker's id and
// depth buffers are shown in the bottom right corner.
//
// DEVIATIONS:
// - The picker reads its buffers back synchronously (getSelection, getWorldPoint) where upstream's
//   are promises; the areas show no object counts, and the buffer previews have no captions.
// - The outlines are camera-parented unlit rods on the IMMEDIATE layer (upstream draws HTML boxes
//   over the canvas); the previews are TexturePreviewPasses (upstream's TextureRenderer).
// - The scattering comes from a fixed-seed generator (mulberry32, seed 1) rather than
//   Math.random(), so every run is the same.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "extras/script/cameraControls.h"
#include "../exampleApp.h"
#include "../texturePreview.h"
#include "framework/components/light/lightComponent.h"
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
// distance so they stay glued to the given window-pixel rectangle.
class RectOutline
{
public:
    RectOutline(Engine* engine, Entity* cameraEntity, CameraComponent* cameraComponent, const Color& color)
        : _cameraComponent(cameraComponent)
    {
        _material = std::make_shared<StandardMaterial>();
        // A flat colour, as an immediate line draws: no light, ambient or fog.
        _material->setUnlit(true);
        _material->setDiffuse(color);
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
    CameraComponent* _cameraComponent = nullptr;
    std::shared_ptr<StandardMaterial> _material;
    std::array<Entity*, 4> _rods{};
};

namespace
{
    struct PickArea
    {
        float x;        // fraction of the window, or the mouse's point
        float y;
        float width;    // in points
        float height;
        const char* color;
        bool mouse;
    };
    constexpr std::array<PickArea, 4> kAreas = {{
        {0.3f, 0.3f, 100.0f, 200.0f, "#ffb340", false},
        {0.6f, 0.7f, 200.0f, 20.0f, "#38d9ff", false},
        {0.6f, 0.4f, 16.0f, 16.0f, "#ff5ce1", false},
        {0.0f, 0.0f, 1.0f, 1.0f, "#ff4d4d", true},
    }};
}

class AreaPickerExample final: public ExampleApp
{
public:
    AreaPickerExample()
        : ExampleApp({.title = "Area Picker", .width = WINDOW_WIDTH, .height = WINDOW_HEIGHT}) {}

protected:
    bool create() override
    {
        _env = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE, assetPath("cubemaps/studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        if (Texture* env = _env->resourceAs<Texture>()) {
            scene()->setEnvAtlas(env);
        }
        scene()->setSkyboxIntensity(0.2f);

        // Neutral clay colours, so the highlight colours stand out.
        const std::array<const char*, 5> palette = {"#e6e1d8", "#bdb8b0", "#8e939b", "#5f656e", "#3c4149"};
        const std::array<const char*, 6> shapes = {"box", "sphere", "cylinder", "capsule", "cone", "torus"};
        uint32_t seed = 1;
        const auto unit = [&seed] {
            // mulberry32
            seed += 0x6D2B79F5u;
            uint32_t t = (seed ^ (seed >> 15)) * (seed | 1u);
            t = (t + ((t ^ (t >> 7)) * (t | 61u))) ^ t;
            return static_cast<float>(static_cast<double>(t ^ (t >> 14)) / 4294967296.0);
        };
        constexpr float radius = 16.0f;
        for (int i = 0; i < 300; ++i) {
            Vector3 position;
            do {
                position = Vector3(unit() * 2.0f - 1.0f, unit() * 2.0f - 1.0f, unit() * 2.0f - 1.0f);
            } while (position.lengthSquared() > 1.0f);
            const char* shape = shapes[static_cast<size_t>(unit() * shapes.size()) % shapes.size()];
            // Each object has its own material, as the highlight changes its emissive colour.
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color().fromString(palette[static_cast<size_t>(unit() * palette.size()) % palette.size()]));
            material->setUseMetalness(true);
            material->setMetalness(0.0f);
            material->setGloss(0.45f);
            _primitiveMaterials.push_back(material);
            Entity* entity = createPrimitive(shape, material.get(), position * radius);
            entity->setLocalEulerAngles(unit() * 360.0f, unit() * 360.0f, unit() * 360.0f);
            const float scale = 0.8f + unit() * 0.8f;
            entity->setLocalScale(scale, scale, scale);
        }

        // The marker of the picked world point, which is not pickable itself.
        auto markerMaterial = std::make_shared<StandardMaterial>();
        markerMaterial->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        markerMaterial->setEmissive(Color().fromString("#7dff6b"));
        markerMaterial->setEmissiveIntensity(10.0f);
        _primitiveMaterials.push_back(markerMaterial);
        _marker = createPrimitive("sphere", markerMaterial.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(0.35f, 0.35f, 0.35f));
        if (auto* render = _marker->findComponent<RenderComponent>()) {
            for (auto* meshInstance : render->meshInstances()) {
                meshInstance->setPick(false);
            }
        }
        _marker->setEnabled(false);

        // The camera auto-orbits until the first right-click.
        _cameraEntity = createCamera(Vector3(0.0f, 0.0f, CAMERA_ORBIT_RADIUS));
        _cameraComponent = _cameraEntity->findComponent<CameraComponent>();
        Color clear;
        clear.fromString("#171b23");
        clear.linear();
        _cameraComponent->camera()->setClearColor(clear);
        _cameraComponent->setLayers({LAYERID_WORLD, LAYERID_IMMEDIATE, LAYERID_UI});
        _cameraComponent->setToneMapping(TONEMAP_NEUTRAL);
        auto rendering = _cameraComponent->rendering();
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 5;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.4f;
        _cameraComponent->setRendering(rendering);

        // The lights ride with the camera, so the lighting looks the same from every side.
        Entity* key = createCameraLight("#ffe0bd", 2.2f, Vector3(50.0f, -40.0f, 0.0f));
        if (auto* light = key->findComponent<LightComponent>()) {
            light->setCastShadows(true);
            light->setShadowType(SHADOW_PCF5_32F);
            light->setShadowResolution(2048);
            light->setShadowBias(0.2f);
            light->setShadowNormalBias(0.05f);
            light->setShadowBounds(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(18.0f, 18.0f, 18.0f)));
        }
        createCameraLight("#7fa8ff", 1.6f, Vector3(-50.0f, -30.0f, 0.0f));

        // A quarter-resolution picker, with depth for the world point.
        _picker = std::make_unique<Picker>(engine(),
            static_cast<int>(WINDOW_WIDTH * PICKER_SCALE),
            static_cast<int>(WINDOW_HEIGHT * PICKER_SCALE), true);

        for (const auto& area : kAreas) {
            _outlines.push_back(std::make_unique<RectOutline>(engine(), _cameraEntity, _cameraComponent,
                Color().fromString(area.color)));
        }

        spdlog::info("Area picker: LMB = pick a world point (green marker), RMB = take over the camera, Esc = quit.");
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            _mouseX = event.motion.x;
            _mouseY = event.motion.y;
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            _pendingWorldPick = std::make_pair(static_cast<int>(event.button.x * PICKER_SCALE),
                static_cast<int>(event.button.y * PICKER_SCALE));
            return true;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT) {
            if (_autoRotate) {
                _autoRotate = false;
                // CameraControls attaches at the camera's current pose (no jump).
                addOrbitControls(_cameraEntity, Vector3(0.0f, 0.0f, 0.0f));
            }
            return true;
        }
        return false;
    }

    void update(const float dt) override
    {
        if (_autoRotate) {
            _time += dt * 0.1f;
            _cameraEntity->setLocalPosition(CAMERA_ORBIT_RADIUS * std::sin(_time), 0.0f,
                CAMERA_ORBIT_RADIUS * std::cos(_time));
            _cameraEntity->lookAt(Vector3(0.0f, 0.0f, 0.0f));
        }

        const auto [windowW, windowH] = engine()->canvasSize();
        _picker->resize(std::max(1, static_cast<int>(static_cast<float>(windowW) * PICKER_SCALE)),
            std::max(1, static_cast<int>(static_cast<float>(windowH) * PICKER_SCALE)));
        _picker->prepare(_cameraComponent, scene().get(), _pickerLayers);

        // Take the previous highlights off.
        for (auto* material : _highlightedMaterials) {
            material->setEmissive(Color(0.0f, 0.0f, 0.0f, 1.0f));
        }
        _highlightedMaterials.clear();

        const float fw = static_cast<float>(windowW);
        const float fh = static_cast<float>(windowH);
        for (size_t i = 0; i < kAreas.size(); ++i) {
            const PickArea& area = kAreas[i];
            const float x = area.mouse ? _mouseX : area.x * fw;
            const float y = area.mouse ? _mouseY : area.y * fh;
            _outlines[i]->update(x, y, area.width, area.height, fw, fh);

            const Color highlight = Color().fromString(area.color);
            const auto selection = _picker->getSelection(static_cast<int>(x * PICKER_SCALE),
                static_cast<int>(y * PICKER_SCALE), std::max(1, static_cast<int>(area.width * PICKER_SCALE)),
                std::max(1, static_cast<int>(area.height * PICKER_SCALE)));
            for (auto* meshInstance : selection) {
                if (auto* material = meshInstance ? dynamic_cast<StandardMaterial*>(meshInstance->material()) : nullptr) {
                    material->setEmissive(highlight);
                    material->setEmissiveIntensity(2.0f);
                    _highlightedMaterials.push_back(material);
                }
            }
        }

        if (_pendingWorldPick) {
            const auto [pickX, pickY] = *_pendingWorldPick;
            _pendingWorldPick.reset();
            const auto worldPoint = _picker->getWorldPoint(pickX, pickY);
            _marker->setEnabled(worldPoint.has_value());
            if (worldPoint) {
                _marker->setPosition(*worldPoint);
            }
        }

        // The picker's buffers side by side in the bottom right corner.
        updatePreview(_colorPreview, _picker->colorBuffer(), 0.725f, fw, fh);
        updatePreview(_depthPreview, _picker->depthBuffer(), 0.835f, fw, fh);
    }

    void destroy() override
    {
        for (auto* preview : {&_colorPreview, &_depthPreview}) {
            if (*preview) {
                engine()->renderer()->removeAppendPass(*preview);
                preview->reset();
            }
        }
        // Both borrow the engine's entities and render targets.
        _outlines.clear();
        _picker.reset();
    }

private:
    Entity* createCameraLight(const char* color, const float intensity, const Vector3& angles)
    {
        auto* entity = new Entity();
        entity->setName("Light");
        entity->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(entity->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            light->setColor(Color().fromString(color));
            light->setIntensity(intensity);
        }
        entity->setLocalEulerAngles(angles.getX(), angles.getY(), angles.getZ());
        _cameraEntity->addChild(entity);
        return entity;
    }

    void updatePreview(std::shared_ptr<TexturePreviewPass>& preview, Texture* texture, const float x, const float fw,
        const float fh)
    {
        if (!texture) {
            return;
        }
        if (!preview) {
            preview = std::make_shared<TexturePreviewPass>(device(), texture);
            preview->init(nullptr);
            engine()->renderer()->addAppendPass(preview);
        }
        preview->setTexture(texture);
        preview->setViewport(Vector4(x * fw, 0.835f * fh, 0.1f * fw, 0.1f * fh));
        preview->setScissor(preview->viewport());
    }

    std::unique_ptr<Asset> _env;
    std::vector<std::shared_ptr<StandardMaterial>> _primitiveMaterials;
    std::vector<std::unique_ptr<RectOutline>> _outlines;
    std::unique_ptr<Picker> _picker;
    std::shared_ptr<TexturePreviewPass> _colorPreview;
    std::shared_ptr<TexturePreviewPass> _depthPreview;

    Entity* _cameraEntity = nullptr;
    CameraComponent* _cameraComponent = nullptr;
    Entity* _marker = nullptr;

    std::vector<int> _pickerLayers = {LAYERID_WORLD};
    std::vector<StandardMaterial*> _highlightedMaterials;
    std::optional<std::pair<int, int>> _pendingWorldPick;
    float _mouseX = 0.0f;
    float _mouseY = 0.0f;

    // Auto-orbit until the first right-click hands control to CameraControls.
    bool _autoRotate = true;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(AreaPickerExample)
