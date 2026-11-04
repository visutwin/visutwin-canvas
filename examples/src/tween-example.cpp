// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream animation/tween.
//
// The eleven easing functions of tween.js, each on a card floating in front of a wall: Linear,
// Quadratic, Cubic, Quartic, Quintic, Sinusoidal, Exponential, Circular, Elastic, Back and Bounce,
// in their in-out form. A card (a rounded box) shows the function's name and type, faint guides at
// the start and end values and the curve itself as a glowing ribbon, coloured along a teal to amber
// gradient across the cards. A playhead moves linearly over the plot carrying a dot up to the eased
// value, so together they trace the curve, and a glossy puck slides along a track beside it to the
// same value, raised above the card to shadow it. Every tween lasts 2 seconds and plays forwards and
// backwards forever, holding half a second at each end. The cards are laid out in as many columns
// as fit the window best; a VSM-shadowed key light from the upper left casts their shadows on the
// wall, fitted to the cards. The camera (fov 30) turns a few degrees toward the pointer; the camera
// frame (4x MSAA, neutral tone mapping) adds bloom and a vignette, the studio environment
// (intensity 0.35) lighting the scene without being seen.
//
// DEVIATIONS:
// - The easing functions are tween.js's in-out curves written here, and the tweens are driven by
//   the update, where upstream loads tween.js and an example script that drives it.
// - The labels are laid out at font size 64 and scaled to upstream's 0.2 units.
// - The layout fits the window at start (no resize handling).
//
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/primitiveGeometry.h"
#include "framework/components/render/renderComponent.h"
#include "scene/materials/standardMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kLabelFontSize = 64;
    constexpr float kLabelWorldSize = 0.2f;

    // The tweens play forwards and backwards forever, holding at each end.
    constexpr float kDuration = 2.0f;
    constexpr float kHold = 0.5f;

    // The size of a card and the gap between the cards.
    constexpr float kCardWidth = 3.0f;
    constexpr float kCardHeight = 2.2f;
    constexpr float kCardDepth = 0.12f;
    constexpr float kGap = 0.3f;
    // The cards float in front of the wall, their shadows showing the distance.
    constexpr float kCardZ = 0.5f;
    constexpr float kFaceZ = kCardZ + kCardDepth / 2.0f;
    // The plot of the curve on a card: 0 at the bottom to 1 at the top over the tween, left to
    // right. Some functions overshoot both ends.
    constexpr float kPlotLeft = -1.25f;
    constexpr float kPlotRight = 0.7f;
    constexpr float kPlotBottom = -0.6f;
    constexpr float kPlotTop = 0.4f;
    constexpr float kTrackX = 1.1f;
    constexpr float kMargin = 0.6f;
    constexpr float kFov = 30.0f;

    float bounceOut(float k)
    {
        if (k < 1.0f / 2.75f) {
            return 7.5625f * k * k;
        }
        if (k < 2.0f / 2.75f) {
            k -= 1.5f / 2.75f;
            return 7.5625f * k * k + 0.75f;
        }
        if (k < 2.5f / 2.75f) {
            k -= 2.25f / 2.75f;
            return 7.5625f * k * k + 0.9375f;
        }
        k -= 2.625f / 2.75f;
        return 7.5625f * k * k + 0.984375f;
    }

    // tween.js's easing curves, in their in-out form
    struct Easing
    {
        const char* name;
        float (*inOut)(float);
    };

    const std::array<Easing, 11> kEasings{{
        {"Linear", [](const float k) { return k; }},
        {"Quadratic", [](float k) {
            k *= 2.0f;
            if (k < 1.0f) return 0.5f * k * k;
            k -= 1.0f;
            return -0.5f * (k * (k - 2.0f) - 1.0f);
        }},
        {"Cubic", [](float k) {
            k *= 2.0f;
            if (k < 1.0f) return 0.5f * k * k * k;
            k -= 2.0f;
            return 0.5f * (k * k * k + 2.0f);
        }},
        {"Quartic", [](float k) {
            k *= 2.0f;
            if (k < 1.0f) return 0.5f * k * k * k * k;
            k -= 2.0f;
            return -0.5f * (k * k * k * k - 2.0f);
        }},
        {"Quintic", [](float k) {
            k *= 2.0f;
            if (k < 1.0f) return 0.5f * k * k * k * k * k;
            k -= 2.0f;
            return 0.5f * (k * k * k * k * k + 2.0f);
        }},
        {"Sinusoidal", [](const float k) { return 0.5f * (1.0f - std::cos(std::numbers::pi_v<float> * k)); }},
        {"Exponential", [](float k) {
            if (k <= 0.0f) return 0.0f;
            if (k >= 1.0f) return 1.0f;
            k *= 2.0f;
            if (k < 1.0f) return 0.5f * std::pow(1024.0f, k - 1.0f);
            return 0.5f * (-std::pow(2.0f, -10.0f * (k - 1.0f)) + 2.0f);
        }},
        {"Circular", [](float k) {
            k *= 2.0f;
            if (k < 1.0f) return -0.5f * (std::sqrt(1.0f - k * k) - 1.0f);
            k -= 2.0f;
            return 0.5f * (std::sqrt(1.0f - k * k) + 1.0f);
        }},
        {"Elastic", [](float k) {
            if (k <= 0.0f) return 0.0f;
            if (k >= 1.0f) return 1.0f;
            k *= 2.0f;
            const float wave = std::sin((k - 1.1f) * 5.0f * std::numbers::pi_v<float>);
            if (k < 1.0f) return -0.5f * std::pow(2.0f, 10.0f * (k - 1.0f)) * wave;
            return 0.5f * std::pow(2.0f, -10.0f * (k - 1.0f)) * wave + 1.0f;
        }},
        {"Back", [](float k) {
            constexpr float s = 1.70158f * 1.525f;
            k *= 2.0f;
            if (k < 1.0f) return 0.5f * (k * k * ((s + 1.0f) * k - s));
            k -= 2.0f;
            return 0.5f * (k * k * ((s + 1.0f) * k + s) + 2.0f);
        }},
        {"Bounce", [](const float k) {
            if (k < 0.5f) return (1.0f - bounceOut(1.0f - k * 2.0f)) * 0.5f;
            return bounceOut(k * 2.0f - 1.0f) * 0.5f + 0.5f;
        }},
    }};

    // A position in the teal to amber gradient across the cards, 0 - 1.
    Color paletteColor(const float t)
    {
        static const std::array<const char*, 7> hex = {"#2dd4bf", "#38bdf8", "#818cf8", "#c084fc", "#f472b6",
            "#fb7185", "#fbbf24"};
        const float position = t * static_cast<float>(hex.size() - 1);
        const size_t i = std::min(static_cast<size_t>(position), hex.size() - 2);
        const float f = position - static_cast<float>(i);
        const Color a = Color().fromString(hex[i]);
        const Color b = Color().fromString(hex[i + 1]);
        return Color(a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, 1.0f);
    }

    // A flat ribbon along a line, facing +Z, its joints mitered so it keeps its width around bends.
    PrimitiveGeometry ribbon(const std::vector<std::array<float, 2>>& points, const float width)
    {
        PrimitiveGeometry geometry;
        const size_t last = points.size() - 1;
        for (size_t i = 0; i < points.size(); ++i) {
            const auto [x, y] = points[i];
            const auto [x0, y0] = points[i > 0 ? i - 1 : 0];
            const auto [x1, y1] = points[std::min(i + 1, last)];
            const float inLength = std::max(std::hypot(x - x0, y - y0), 1e-12f);
            const float outLength = std::max(std::hypot(x1 - x, y1 - y), 1e-12f);
            const float inX = i > 0 ? (x - x0) / inLength : (x1 - x) / outLength;
            const float inY = i > 0 ? (y - y0) / inLength : (y1 - y) / outLength;
            const float outX = i < last ? (x1 - x) / outLength : inX;
            const float outY = i < last ? (y1 - y) / outLength : inY;
            // The miter is perpendicular to the average direction, longer the sharper the bend.
            const float tangentLength = std::hypot(inX + outX, inY + outY);
            const float nx = -(inY + outY) / tangentLength;
            const float ny = (inX + outX) / tangentLength;
            const float miter = std::min(width / 2.0f / std::max(nx * -inY + ny * inX, 0.1f), width * 2.0f);
            geometry.positions.insert(geometry.positions.end(),
                {x - nx * miter, y - ny * miter, 0.0f, x + nx * miter, y + ny * miter, 0.0f});
            geometry.normals.insert(geometry.normals.end(), {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f});
            geometry.uvs.insert(geometry.uvs.end(), {0.0f, 0.0f, 0.0f, 1.0f});
            if (i < last) {
                const auto v = static_cast<uint32_t>(i * 2);
                geometry.indices.insert(geometry.indices.end(), {v, v + 2, v + 1, v + 1, v + 2, v + 3});
            }
        }
        return geometry;
    }

    PrimitiveGeometry roundedBox(const float hx, const float hy, const float hz, const float radius, const int segments)
    {
        RoundedBoxGeometryOptions options;
        options.halfExtents[0] = hx;
        options.halfExtents[1] = hy;
        options.halfExtents[2] = hz;
        options.radius = radius;
        options.radiusSegments = segments;
        return createRoundedBoxGeometry(options);
    }

    // The eased value of a tween at time t: forwards over kDuration, a hold, backwards, a hold.
    float tweenValue(const Easing& easing, const float t)
    {
        const float cycle = 2.0f * (kDuration + kHold);
        const float phase = std::fmod(t, cycle);
        if (phase < kDuration) {
            return easing.inOut(phase / kDuration);
        }
        if (phase < kDuration + kHold) {
            return 1.0f;
        }
        if (phase < 2.0f * kDuration + kHold) {
            return 1.0f - easing.inOut((phase - kDuration - kHold) / kDuration);
        }
        return 0.0f;
    }
}

class TweenExample final : public ExampleApp
{
public:
    TweenExample() : ExampleApp({.title = "Tween"}) {}

protected:
    void configure(AppOptions& options) override
    {
        // The labels are world-space text elements, on no screen.
        registerUi(options, {.screen = false});
    }

    bool create() override
    {
        _envAsset = std::make_unique<Asset>("env-atlas", AssetType::TEXTURE, assetPath("cubemaps/studio-env-atlas.png"),
            AssetData{.type = TextureType::TEXTURETYPE_RGBP, .mipmaps = false});
        _boldAsset = std::make_unique<Asset>("bold-font", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        _lightAsset = std::make_unique<Asset>("light-font", AssetType::FONT, assetPath("fonts/roboto-extralight.json"));
        auto* boldFont = _boldAsset->resourceAs<FontResource>();
        auto* lightFont = _lightAsset->resourceAs<FontResource>();
        if (Texture* env = _envAsset->resourceAs<Texture>()) {
            scene()->setEnvAtlas(env);
        }
        scene()->setSkyboxIntensity(0.35f);
        if (!boldFont || !lightFont) {
            spdlog::error("tween needs fonts/roboto-bold.json and roboto-extralight.json");
            return false;
        }

        // The wall behind the cards, catching their shadows.
        auto wallMaterial = createMaterial(Color().fromString("#1b202b"), 0.2f);
        Entity* wall = createPrimitive("plane", wallMaterial.get(), Vector3(0.0f, 0.0f, 0.0f), Vector3(200.0f, 1.0f, 200.0f));
        wall->setLocalEulerAngles(90.0f, 0.0f, 0.0f);
        if (auto* render = wall->findComponent<RenderComponent>()) {
            render->setCastShadows(false);
        }

        auto cardMaterial = createMaterial(Color().fromString("#2a3142"), 0.45f);
        auto trackMaterial = createMaterial(Color().fromString("#161a23"), 0.3f);
        auto guideMaterial = createGlowMaterial(Color().fromString("#6a7894"), 1.0f);
        const Color nameColor = Color().fromString("#e6ebf3");
        const Color typeColor = Color().fromString("#8792a8");

        auto cardMesh = createMeshFromGeometry(device(),
            roundedBox(kCardWidth / 2.0f, kCardHeight / 2.0f, kCardDepth / 2.0f, 0.12f, 6));
        auto trackMesh = createMeshFromGeometry(device(),
            roundedBox(0.045f, (kPlotTop - kPlotBottom) / 2.0f + 0.045f, 0.012f, 0.012f, 4));
        auto puckMesh = createMeshFromGeometry(device(), roundedBox(0.15f, 0.15f, 0.07f, 0.065f, 8));
        auto guideMesh = createMeshFromGeometry(device(), ribbon({{kPlotLeft, 0.0f}, {kPlotRight, 0.0f}}, 0.018f));
        auto playheadMesh = createMeshFromGeometry(device(),
            ribbon({{0.0f, kPlotBottom - 0.16f}, {0.0f, kPlotTop + 0.16f}}, 0.016f));

        constexpr int kSamples = 160;
        for (size_t i = 0; i < kEasings.size(); ++i) {
            const Easing& easing = kEasings[i];
            const Color color = paletteColor(static_cast<float>(i) / static_cast<float>(kEasings.size() - 1));
            Card card;
            card.easing = &easing;
            card.entity = createMeshEntity(easing.name, cardMesh, cardMaterial, root(), true);

            // The name of the function and its type, as in TWEEN.Easing.Cubic.InOut.
            createLabel(card.entity, boldFont, easing.name, kPlotLeft, 0.0f, nameColor);
            createLabel(card.entity, lightFont, i == 0 ? "None" : "InOut", kCardWidth / 2.0f - 0.25f, 1.0f, typeColor);

            // Faint guides at the start and end values.
            for (const float y : {kPlotBottom, kPlotTop}) {
                createMeshEntity("Guide", guideMesh, guideMaterial, card.entity, false)
                    ->setLocalPosition(0.0f, y, kCardDepth / 2.0f + 0.001f);
            }

            // The glowing curve.
            std::vector<std::array<float, 2>> points;
            for (int sample = 0; sample <= kSamples; ++sample) {
                const float t = static_cast<float>(sample) / kSamples;
                points.push_back({kPlotLeft + (kPlotRight - kPlotLeft) * t,
                    kPlotBottom + (kPlotTop - kPlotBottom) * easing.inOut(t)});
            }
            _meshes.push_back(createMeshFromGeometry(device(), ribbon(points, 0.035f)));
            createMeshEntity("Curve", _meshes.back(), createGlowMaterial(color, 2.5f), card.entity, false)
                ->setLocalPosition(0.0f, 0.0f, kCardDepth / 2.0f + 0.003f);

            // The playhead moves linearly over the plot; its dot moves up to the eased value.
            card.playhead = createMeshEntity("Playhead", playheadMesh, guideMaterial, card.entity, false);
            const Color dotColor(color.r + (1.0f - color.r) * 0.5f, color.g + (1.0f - color.g) * 0.5f,
                color.b + (1.0f - color.b) * 0.5f, 1.0f);
            card.dot = createPrimitive("sphere", createGlowMaterial(dotColor, 6.0f).get());
            card.dot->setName("Dot");
            if (auto* render = card.dot->findComponent<RenderComponent>()) {
                render->setCastShadows(false);
            }
            card.playhead->addChild(card.dot);
            card.dot->setLocalScale(0.13f, 0.13f, 0.13f);

            // The puck slides along its track, raised above the card to shadow it.
            createMeshEntity("Track", trackMesh, trackMaterial, card.entity, false)
                ->setLocalPosition(kTrackX, (kPlotBottom + kPlotTop) / 2.0f, kCardDepth / 2.0f);
            auto puckMaterial = createMaterial(color, 0.8f);
            puckMaterial->setClearCoat(0.5f);
            puckMaterial->setClearCoatGloss(0.9f);
            card.puck = createMeshEntity("Puck", puckMesh, puckMaterial, card.entity, true);
            if (auto* render = card.puck->findComponent<RenderComponent>()) {
                render->setReceiveShadows(false);
            }
            _cards.push_back(card);
        }

        // The key light from the upper left: soft VSM shadows of the cards on the wall and of the
        // pucks on the cards. It shines along its -Y axis, turned to -Z, looking down and right.
        _light = new Entity();
        _light->setName("Light");
        _light->setEngine(engine());
        if (auto* light = static_cast<LightComponent*>(_light->addComponent<LightComponent>())) {
            light->setType(LightType::LIGHTTYPE_DIRECTIONAL);
            light->setColor(Color(1.0f, 0.97f, 0.92f, 1.0f));
            light->setIntensity(1.0f);
            light->setCastShadows(true);
            light->setShadowType(SHADOW_VSM_16F);
            light->setShadowResolution(2048);
            light->setVsmBlurSize(15);
        }
        root()->addChild(_light);
        _light->lookAt(0.45f, -0.6f, -1.0f);
        _light->rotateLocal(90.0f, 0.0f, 0.0f);

        _camera = createCamera(Vector3(0.0f, 0.0f, 20.0f));
        auto* cameraComponent = _camera->findComponent<CameraComponent>();
        cameraComponent->camera()->setFov(kFov);
        cameraComponent->camera()->setNearClip(0.5f);
        cameraComponent->camera()->setFarClip(100.0f);
        cameraComponent->setLayers({LAYERID_WORLD, LAYERID_UI});
        cameraComponent->setToneMapping(TONEMAP_NEUTRAL);
        auto rendering = cameraComponent->rendering();
        rendering.samples = 4;
        rendering.bloomIntensity = 0.03f;
        rendering.bloomBlurLevel = 5;
        rendering.vignetteEnabled = true;
        rendering.vignetteIntensity = 0.45f;
        rendering.vignetteInner = 0.4f;
        rendering.vignetteOuter = 1.3f;
        cameraComponent->setRendering(rendering);

        layout();
        update(0.0f);
        return true;
    }

    bool onEvent(const SDL_Event& event) override
    {
        // The camera turns toward the pointer.
        if (event.type == SDL_EVENT_MOUSE_MOTION) {
            const auto [width, height] = engine()->canvasSize();
            _targetYaw = (event.motion.x / static_cast<float>(std::max(width, 1)) * 2.0f - 1.0f) * 6.0f;
            _targetPitch = (1.0f - event.motion.y / static_cast<float>(std::max(height, 1)) * 2.0f) * 4.0f;
        }
        return false;
    }

    void update(const float dt) override
    {
        _time += dt;
        for (const auto& card : _cards) {
            const float linear = tweenValue(kEasings[0], _time);
            const float eased = tweenValue(*card.easing, _time);
            card.playhead->setLocalPosition(kPlotLeft + (kPlotRight - kPlotLeft) * linear, 0.0f, kCardDepth / 2.0f + 0.002f);
            card.dot->setLocalPosition(0.0f, kPlotBottom + (kPlotTop - kPlotBottom) * eased, 0.03f);
            card.puck->setLocalPosition(kTrackX, kPlotBottom + (kPlotTop - kPlotBottom) * eased, kCardDepth / 2.0f + 0.16f);
        }

        // The camera turns around the cards' centre toward the pointer, smoothed.
        const float blend = 1.0f - std::exp(-4.0f * dt);
        _yaw += (_targetYaw - _yaw) * blend;
        _pitch += (_targetPitch - _pitch) * blend;
        constexpr float toRad = std::numbers::pi_v<float> / 180.0f;
        const float yaw = _yaw * toRad;
        const float pitch = _pitch * toRad;
        _camera->setLocalPosition(std::sin(yaw) * std::cos(pitch) * _cameraDistance,
            std::sin(pitch) * _cameraDistance, kFaceZ + std::cos(yaw) * std::cos(pitch) * _cameraDistance);
        _camera->lookAt(Vector3(0.0f, 0.0f, kFaceZ));
    }

private:
    struct Card
    {
        const Easing* easing = nullptr;
        Entity* entity = nullptr;
        Entity* playhead = nullptr;
        Entity* dot = nullptr;
        Entity* puck = nullptr;
    };

    std::shared_ptr<StandardMaterial> createMaterial(const Color& color, const float gloss)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(color);
        material->setUseMetalness(true);
        material->setMetalness(0.0f);
        material->setGloss(gloss);
        _materials.push_back(material);
        return material;
    }

    // Unlit, glowing when its intensity is above 1.
    std::shared_ptr<StandardMaterial> createGlowMaterial(const Color& color, const float intensity)
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setUseLighting(false);
        material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        material->setEmissive(color);
        material->setEmissiveIntensity(intensity);
        material->setCullMode(CullMode::CULLFACE_NONE);
        _materials.push_back(material);
        return material;
    }

    Entity* createMeshEntity(const char* name, const std::shared_ptr<Mesh>& mesh, const std::shared_ptr<StandardMaterial>& material,
        Entity* parent, const bool castShadows)
    {
        auto* entity = new Entity();
        entity->setName(name);
        entity->setEngine(engine());
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        render->addMeshInstance(std::make_unique<MeshInstance>(mesh, material, entity));
        render->setCastShadows(castShadows);
        parent->addChild(entity);
        return entity;
    }

    void createLabel(Entity* card, FontResource* font, const char* text, const float x, const float pivotX,
        const Color& color)
    {
        ElementComponent* element = createElement(engine(), card, {
            .type = ElementType::Text,
            .color = color,
            .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
            .pivot = Vector2(pivotX, 0.5f),
            .font = font,
            .text = text,
            .fontSize = kLabelFontSize
        });
        element->setLayers({LAYERID_WORLD});
        constexpr float scale = kLabelWorldSize / static_cast<float>(kLabelFontSize);
        element->entity()->setLocalPosition(x, 0.8f, kCardDepth / 2.0f + 0.002f);
        element->entity()->setLocalScale(scale, scale, scale);
    }

    // As many columns as fit the window best (the last row centred), and the camera distance that
    // fits all of them.
    void layout()
    {
        const auto [width, height] = engine()->canvasSize();
        const float aspect = static_cast<float>(width) / static_cast<float>(std::max(height, 1));
        const float tanHalfFov = std::tan(kFov * 0.5f * std::numbers::pi_v<float> / 180.0f);
        const int count = static_cast<int>(_cards.size());
        int columns = 2;
        int rows = 1;
        float layoutWidth = 0.0f;
        float layoutHeight = 0.0f;
        _cameraDistance = std::numeric_limits<float>::max();
        for (int c = 2; c <= 6; ++c) {
            const int r = (count + c - 1) / c;
            const float w = static_cast<float>(c) * kCardWidth + static_cast<float>(c - 1) * kGap;
            const float h = static_cast<float>(r) * kCardHeight + static_cast<float>(r - 1) * kGap;
            const float distance = std::max((h / 2.0f + kMargin) / tanHalfFov, (w / 2.0f + kMargin) / (tanHalfFov * aspect));
            if (distance < _cameraDistance) {
                _cameraDistance = distance;
                columns = c;
                rows = r;
                layoutWidth = w;
                layoutHeight = h;
            }
        }
        for (int i = 0; i < count; ++i) {
            const int row = i / columns;
            const int inRow = std::min(columns, count - row * columns);
            const int column = i % columns;
            _cards[static_cast<size_t>(i)].entity->setLocalPosition(
                (static_cast<float>(column) - static_cast<float>(inRow - 1) / 2.0f) * (kCardWidth + kGap),
                (static_cast<float>(rows - 1) / 2.0f - static_cast<float>(row)) * (kCardHeight + kGap), kCardZ);
        }
        if (auto* light = _light->findComponent<LightComponent>()) {
            light->setShadowBounds(BoundingBox(Vector3(0.0f, 0.0f, kFaceZ),
                Vector3(layoutWidth / 2.0f, layoutHeight / 2.0f, 0.4f)));
        }
    }

    std::unique_ptr<Asset> _envAsset;
    std::unique_ptr<Asset> _boldAsset;
    std::unique_ptr<Asset> _lightAsset;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<std::shared_ptr<Mesh>> _meshes;
    std::vector<Card> _cards;
    Entity* _light = nullptr;
    Entity* _camera = nullptr;
    float _cameraDistance = 20.0f;
    float _targetYaw = 0.0f;
    float _targetPitch = 0.0f;
    float _yaw = 0.0f;
    float _pitch = 0.0f;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TweenExample)
