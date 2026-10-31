// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// Port of upstream test/scene-depth-read (a hidden test example).
//
// A functional test of SceneDepthReader. Three boxes (2 units) stand with their front faces 10, 15
// and 20 units from an orthographic camera (height 6, near 0.1, far 100), each at a height of its
// own, so a read mirrored vertically would land on empty space; the depth of each is read back over
// a 4 x 4 patch of samples centred on it, and a fourth patch is read from the empty space above
// them. Every sample on a box must be its distance (to half a unit) and every sample above them
// infinite. Run once for each producer of the camera's depth: the depth grab
// (requestSceneDepthMap), a camera frame's scene pass (depth of field, single-sampled), and its
// depth prepass (the same at 4x MSAA). The encoding each delivered is reported beside the result,
// and the whole run ends PASSED or FAILED, on screen and in the log.
//
// DEVIATIONS:
// - Every producer here stores depth buffer values, so all three phases report the non-linear
//   encoding, where upstream's scene pass stores reciprocal depth and its prepass linear depth.
//   The three producers are still each read, through the reader's one decode.
// - The camera frame is the camera component's (depth of field, its MSAA sample count) where
//   upstream builds a CameraFrame; the scene-texture phase needs no splat setting.
// - A read is delivered to a callback at a later frame, where upstream returns a promise.
//
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "framework/assets/asset.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "platform/graphics/sceneMapHandles.h"
#include "scene/graphics/sceneDepthReader.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr std::array<float, 3> kDistances{10.0f, 15.0f, 20.0f};
    constexpr float kBoxSize = 2.0f;
    constexpr uint32_t kSamples = 4;
    constexpr float kPatch = 0.02f;
    constexpr int kSettleFrames = 2;

    // Where a box stands: each at a height of its own, its front face at the distance tested
    Vector3 boxPosition(const size_t i)
    {
        const float offset = static_cast<float>(i) - 1.0f;
        return Vector3(offset * 3.0f, offset * 2.5f, -kDistances[i] - kBoxSize / 2.0f);
    }

    struct Phase
    {
        const char* name;
        const char* expect;
    };
    constexpr std::array<Phase, 3> kPhases{{
        // nothing consumes the depth, so it is requested outright: the depth grab
        {"depth grab", "non-linear"},
        // a camera frame's scene pass carries the depth, single-sampled
        {"scene textures", "non-linear"},
        // multisampling rules the scene pass out, which leaves the prepass to render the depth
        {"depth prepass", "non-linear"},
    }};
}

class SceneDepthReadExample final : public ExampleApp
{
public:
    SceneDepthReadExample() : ExampleApp({.title = "Scene Depth Read", .antialias = false}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _fontAsset = std::make_unique<Asset>("font", AssetType::FONT, assetPath("fonts/roboto-regular.json"));
        auto* font = _fontAsset->resourceAs<FontResource>();
        if (!font) {
            spdlog::error("scene-depth-read needs fonts/roboto-regular.json");
            return false;
        }

        // Three boxes, at 10, 15 and 20 units from the camera
        _material = std::make_shared<StandardMaterial>();
        for (size_t i = 0; i < kDistances.size(); ++i) {
            Entity* box = createPrimitive("box", _material.get(), boxPosition(i),
                Vector3(kBoxSize, kBoxSize, kBoxSize));
            box->setName("box-" + std::to_string(i));
        }
        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f);

        // Orthographic, so a sample's depth is its distance along the view direction wherever the
        // box sits on screen, which makes the expected values exact
        Entity* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        _camera = camera->findComponent<CameraComponent>();
        Camera* cam = _camera->camera();
        cam->setClearColor(Color(0.1f, 0.1f, 0.15f, 1.0f));
        cam->setNearClip(0.1f);
        cam->setFarClip(100.0f);
        cam->setProjection(ProjectionType::Orthographic);
        cam->setOrthoHeight(6.0f);

        _reader = std::make_unique<SceneDepthReader>(device(), cam);

        // The report, bottom left
        ScreenComponent* screen = createScreen();
        _report = createElement(engine(), screen->entity(), {
            .type = ElementType::Text,
            .anchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f),
            .pivot = Vector2(0.0f, 0.0f),
            .font = font,
            .text = "Running...",
            .fontSize = 18,
        });
        _report->entity()->setLocalPosition(16.0f, 16.0f, 0.0f);
        return true;
    }

    void update(float /*dt*/) override
    {
        if (_pending || _phase >= kPhases.size()) {
            return;
        }
        if (_framesInPhase == 0) {
            setupPhase(_phase);
        }
        if (++_framesInPhase <= kSettleFrames) {
            return;
        }

        // one read per box, plus one clear of them - which also has several reads in flight at once
        std::vector<Vector4> regions;
        for (size_t i = 0; i < kDistances.size(); ++i) {
            regions.push_back(placeRegion(boxPosition(i)));
        }
        regions.push_back(placeRegion(Vector3(0.0f, 4.5f, -15.0f)));

        _results.assign(regions.size(), {});
        _remaining = regions.size();
        bool queued = true;
        for (size_t i = 0; i < regions.size() && queued; ++i) {
            queued = _reader->read(regions[i], kSamples, kSamples, [this, i](const std::vector<float>& samples) {
                _results[i] = samples;
                if (--_remaining == 0) {
                    finishPhase();
                }
            });
        }
        if (!queued) {
            setReport(std::string(kPhases[_phase].name) + ": waiting for the depth");
            return;
        }
        _pending = true;
    }

private:
    void setupPhase(const size_t phase)
    {
        auto rendering = _camera->rendering();
        auto dof = _camera->dof();
        if (phase == 0) {
            dof.enabled = false;
            rendering.samples = 1;
            _camera->requestSceneDepthMap(true);
        } else {
            if (phase == 1) {
                _camera->requestSceneDepthMap(false);
            }
            // depth of field is what makes the frame render a depth at all
            dof.enabled = true;
            rendering.samples = phase == 1 ? 1 : 4;
        }
        _camera->setDof(dof);
        _camera->setRendering(rendering);
    }

    // A normalized region from the viewport's bottom-left corner, centred on a world position
    Vector4 placeRegion(const Vector3& world) const
    {
        const Vector3 screenPoint = _camera->worldToScreen(world);
        const auto [width, height] = engine()->canvasSize();
        // screen coordinates run down from the top, the regions up from the bottom
        return Vector4(screenPoint.getX() / static_cast<float>(width) - kPatch / 2.0f,
            1.0f - screenPoint.getY() / static_cast<float>(height) - kPatch / 2.0f, kPatch, kPatch);
    }

    std::string encoding() const
    {
        const auto& handle = _camera->camera()->sceneDepthMapHandle();
        if (!handle || !handle->linear) {
            return "non-linear";
        }
        return handle->reciprocal ? "reciprocal" : (handle->packed ? "linear packed" : "linear");
    }

    void finishPhase()
    {
        const Phase& phase = kPhases[_phase];
        const std::string found = encoding();
        bool passed = found == phase.expect;
        std::string text = std::string(phase.name) + ": " + found +
            (passed ? "" : std::string(" - expected ") + phase.expect + ", FALLBACK");
        // the patch sits entirely on the box, so every sample in it is that box's distance
        for (size_t i = 0; i < kDistances.size(); ++i) {
            const auto& samples = _results[i];
            const bool ok = !samples.empty() && std::all_of(samples.begin(), samples.end(),
                [&](const float value) { return std::abs(value - kDistances[i]) < 0.5f; });
            const auto [low, high] = std::minmax_element(samples.begin(), samples.end());
            char line[128];
            std::snprintf(line, sizeof(line), "\n  box at %.0f: samples %.2f to %.2f  %s", kDistances[i],
                samples.empty() ? 0.0f : *low, samples.empty() ? 0.0f : *high, ok ? "ok" : "FAILED");
            text += line;
            passed = passed && ok;
        }
        // and nothing above them, so every sample there is infinite
        const auto& empty = _results.back();
        const bool allEmpty = !empty.empty() && std::all_of(empty.begin(), empty.end(),
            [](const float value) { return std::isinf(value); });
        text += std::string("\n  above the boxes: all infinite  ") + (allEmpty ? "ok" : "FAILED");
        passed = passed && allEmpty;

        spdlog::info("{}", text);
        _reports.push_back(text);
        _allPassed = _allPassed && passed;
        ++_phase;
        _framesInPhase = 0;
        _pending = false;
        std::string report;
        for (const auto& line : _reports) {
            report += line + "\n";
        }
        if (_phase >= kPhases.size()) {
            report += _allPassed ? "\nPASSED" : "\nFAILED";
            spdlog::info("scene-depth-read: {}", _allPassed ? "PASSED" : "FAILED");
        }
        setReport(report);
    }

    void setReport(const std::string& text)
    {
        if (_report) {
            _report->setText(text);
        }
    }

    std::unique_ptr<Asset> _fontAsset;
    std::shared_ptr<StandardMaterial> _material;
    CameraComponent* _camera = nullptr;
    std::unique_ptr<SceneDepthReader> _reader;
    ElementComponent* _report = nullptr;
    std::vector<std::vector<float>> _results;
    std::vector<std::string> _reports;
    size_t _remaining = 0;
    size_t _phase = 0;
    int _framesInPhase = 0;
    bool _pending = false;
    bool _allPassed = true;
};

VISUTWIN_EXAMPLE_MAIN(SceneDepthReadExample)
