// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream animation/tween.
//
// Eleven spheres (red, specular 0.6, gloss 0.2, 0.8 across) tween their local position from x = 0
// to x = 4 and back on rows one unit apart, each with its own easing function in its in-out form:
// Linear, Quadratic, Cubic, Quartic, Quintic, Sinusoidal, Exponential, Circular, Elastic, Back and
// Bounce. A tween lasts 1.5 seconds and repeats forever, ping-ponging between its ends. A white
// line marks each sphere's path and a label on its left names its easing. A directional light
// turned (70, 30, 0); the camera at (0.65, -5.5, 20) clears to (0.4, 0.45, 0.5).
//
// DEVIATIONS:
// - The easing functions are tween.js's in-out curves written here, where upstream loads tween.js
//   and an example script that drives it.
// - The labels are laid out at font size 64 and scaled to upstream's 0.5 units.
//
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kLabelFontSize = 64;
    constexpr float kLabelWorldSize = 0.5f;
    constexpr float kDuration = 1.5f;

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
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        auto* font = _fontAsset->resourceAs<FontResource>();
        if (!font) {
            spdlog::error("tween needs fonts/roboto-regular.json");
            return false;
        }

        for (size_t i = 0; i < kEasings.size(); ++i) {
            const float row = -static_cast<float>(i);
            // A sphere, tweened along its row
            auto material = std::make_shared<StandardMaterial>();
            material->setDiffuse(Color(1.0f, 0.0f, 0.0f, 1.0f));
            material->setSpecular(Color(0.6f, 0.6f, 0.6f, 1.0f));
            material->setGloss(0.2f);
            _materials.push_back(material);
            Entity* sphere = createPrimitive("sphere", material.get(), Vector3(0.0f, row, 0.0f),
                Vector3(0.8f, 0.8f, 0.8f));
            _spheres.push_back(sphere);

            // A line for the path of the sphere
            _lines.push_back({Vector3(0.0f, row, 0.0f), Vector3(4.0f, row, 0.0f)});

            // A label for the sphere, its right edge half a unit left of the path
            ElementComponent* element = createElement(engine(), root(), {
                .type = ElementType::Text,
                .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                .pivot = Vector2(1.0f, 0.5f),
                .font = font,
                .text = kEasings[i].name,
                .fontSize = kLabelFontSize
            });
            constexpr float scale = kLabelWorldSize / static_cast<float>(kLabelFontSize);
            element->entity()->setLocalPosition(-0.5f, row, 0.0f);
            element->entity()->setLocalScale(scale, scale, scale);
        }

        createDirectionalLight(Vector3(70.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f);

        Entity* camera = createCamera(Vector3(0.65f, -5.5f, 20.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        return true;
    }

    void update(const float dt) override
    {
        _time += dt;
        // A tween ping-pongs: forward in even periods, back in odd ones
        const float periods = _time / kDuration;
        const auto period = static_cast<long>(std::floor(periods));
        float k = periods - static_cast<float>(period);
        if (period % 2 == 1) {
            k = 1.0f - k;
        }
        for (size_t i = 0; i < _spheres.size(); ++i) {
            const float x = 4.0f * kEasings[i].inOut(k);
            _spheres[i]->setLocalPosition(x, -static_cast<float>(i), 0.0f);
        }
        for (const auto& [from, to] : _lines) {
            scene()->drawLine(from, to, Color(1.0f, 1.0f, 1.0f, 1.0f));
        }
    }

private:
    std::unique_ptr<Asset> _fontAsset;
    std::vector<std::shared_ptr<StandardMaterial>> _materials;
    std::vector<Entity*> _spheres;
    std::vector<std::pair<Vector3, Vector3>> _lines;
    float _time = 0.0f;
};

VISUTWIN_EXAMPLE_MAIN(TweenExample)
