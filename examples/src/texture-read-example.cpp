// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream test/texture-read (a hidden test example).
//
// A round trip through Texture::read for the normalised 8-bit formats R8, RG8 and RGBA8. For
// each format a 4 x 4 texture (no mipmaps, nearest filtering, clamped addressing) is filled
// with the pattern (i * 17 + 31) % 256 over its bytes, uploaded, read back from the GPU and
// compared byte for byte; the length of what comes back is checked first. Each result goes
// to the log, and the screen shows three overlays on an empty (black) frame: the backend
// ("Device: METAL" or "VULKAN") at the top centre in white on translucent black, the verdict
// in the middle ("ALL TESTS PASSED" on green, "TESTS FAILED" on red, size 32, bold), and
// below it the formats that passed or the first mismatch of each that failed.
//
// The tests run once, from create(), before the first frame: a read is blocking, and
// neither backend can read back while a frame is recording.
//
// DEVIATIONS:
// - the HTML overlays are UI elements on a screen at one unit per window point; their
//   corners are square (no border radius), Arial is Liberation Sans (metrically the same)
//   and the monospace details are Liberation Sans too, since no monospace font ships.
// - a texture is filled through setLevelData and upload() where upstream locks and
//   unlocks it.
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "../uiElements.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/assets/asset.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/components/screen/screenComponentSystem.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/texture.h"

using namespace visutwin::canvas;

namespace
{
    // Test texture size.
    constexpr uint32_t kTexWidth = 4;
    constexpr uint32_t kTexHeight = 4;

    struct FormatToTest
    {
        PixelFormat format;
        const char* name;
        uint32_t channels;
    };

    // The normalised 8-bit formats.
    constexpr FormatToTest kFormatsToTest[] = {
        {PixelFormat::PIXELFORMAT_R8, "R8", 1},
        {PixelFormat::PIXELFORMAT_RG8, "RG8", 2},
        {PixelFormat::PIXELFORMAT_RGBA8, "RGBA8", 4},
    };

    struct TestResult
    {
        std::string name;
        bool passed = false;
        std::string error;
    };

    // A recognisable pattern over the 0-255 range.
    std::vector<uint8_t> generateTestData(const uint32_t width, const uint32_t height, const uint32_t channels)
    {
        std::vector<uint8_t> data(static_cast<size_t>(width) * height * channels);
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<uint8_t>((i * 17 + 31) % 256);
        }
        return data;
    }

    // The two overlays' looks, as the page styles them.
    struct OverlayStyle
    {
        FontResource* font = nullptr;
        int fontSize = 18;
        Color textColor = Color(1.0f, 1.0f, 1.0f, 1.0f);
        Color background = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float backgroundOpacity = 0.7f;
        Vector2 padding = Vector2(16.0f, 8.0f);   // horizontal, vertical
        ElementHorizontalAlign align = ElementHorizontalAlign::Center;
    };

    /// A text on a padded background box.
    struct Overlay
    {
        ElementComponent* panel = nullptr;
        ElementComponent* text = nullptr;
        Vector2 padding;

        void setText(const std::string& value) const
        {
            text->setText(value);
            // The box follows the text, which takes its size at once.
            panel->setWidth(text->width() + 2.0f * padding.x);
            panel->setHeight(text->height() + 2.0f * padding.y);
        }

        void setBackground(const Color& color, const float opacity) const
        {
            panel->setColor(color);
            panel->setOpacity(opacity);
        }
    };
}

class TextureReadExample final: public ExampleApp
{
public:
    TextureReadExample(): ExampleApp({.title = "Texture Read"}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _regular = std::make_unique<Asset>("liberation-sans", AssetType::FONT,
            assetPath("fonts/liberation-sans.json"));
        _bold = std::make_unique<Asset>("roboto-bold", AssetType::FONT, assetPath("fonts/roboto-bold.json"));
        FontResource* regular = _regular->resourceAs<FontResource>();
        FontResource* bold = _bold->resourceAs<FontResource>();
        if (!regular || !bold) {
            spdlog::error("Failed to load fonts/liberation-sans.json or fonts/roboto-bold.json");
            return false;
        }

        // Nothing but the overlays is drawn; the camera stands for the empty canvas.
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.0f, 0.0f, 0.0f, 1.0f));

        // One screen unit per window point, as the page's CSS pixels.
        _screen = createScreen({.scaleMode = ScreenScaleMode::None});
        Entity* screenEntity = _screen->entity();

        // Device info overlay (top centre).
        _deviceInfo = createOverlay(screenEntity, Vector4(0.5f, 1.0f, 0.5f, 1.0f), Vector2(0.5f, 1.0f),
            Vector2(0.0f, -20.0f), {.font = regular, .fontSize = 18});
        _deviceInfo.setText(std::string("Device: ") + backendName());

        // Result overlay (centre).
        _resultOverlay = createOverlay(screenEntity, Vector4(0.5f, 0.5f, 0.5f, 0.5f), Vector2(0.5f, 0.5f),
            Vector2(0.0f, 0.0f), {.font = bold, .fontSize = 32, .padding = Vector2(48.0f, 24.0f)});

        // Details overlay (its top 65% of the way down).
        _detailsOverlay = createOverlay(screenEntity, Vector4(0.5f, 0.35f, 0.5f, 0.35f), Vector2(0.5f, 1.0f),
            Vector2(0.0f, 0.0f), {.font = regular, .fontSize = 14, .padding = Vector2(24.0f, 12.0f),
                                  .align = ElementHorizontalAlign::Left});

        runTests();
        return true;
    }

private:
    const char* backendName() const
    {
        switch (backend()) {
        case Backend::Metal:
            return "METAL";
        case Backend::Vulkan:
            return "VULKAN";
        case Backend::WebGPU:
            return "WEBGPU";
        }
        return "UNKNOWN";
    }

    Overlay createOverlay(Entity* parent, const Vector4& anchor, const Vector2& pivot, const Vector2& position,
        const OverlayStyle& style) const
    {
        Overlay overlay;
        overlay.padding = style.padding;
        overlay.panel = createElement(engine(), parent, {.type = ElementType::Image,
            .color = style.background, .opacity = style.backgroundOpacity, .anchor = anchor, .pivot = pivot});
        overlay.panel->entity()->setLocalPosition(position.x, position.y, 0.0f);
        overlay.text = createElement(engine(), overlay.panel->entity(), {.type = ElementType::Text,
            .color = style.textColor, .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f), .pivot = Vector2(0.5f, 0.5f),
            .font = style.font, .fontSize = style.fontSize, .horizontalAlign = style.align});
        return overlay;
    }

    TestResult testFormat(const FormatToTest& formatInfo) const
    {
        const std::string name = formatInfo.name;
        spdlog::info("Testing format: {}", name);

        // Create the texture.
        TextureOptions options;
        options.name = "Test_" + name;
        options.width = kTexWidth;
        options.height = kTexHeight;
        options.format = formatInfo.format;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        auto texture = std::make_shared<Texture>(device().get(), options);
        texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);

        // Write the data and upload it.
        const std::vector<uint8_t> expectedData = generateTestData(kTexWidth, kTexHeight, formatInfo.channels);
        texture->setLevelData(0, expectedData.data(), expectedData.size());
        texture->upload();

        // Read it back from the GPU.
        std::vector<uint8_t> readData;
        if (!texture->read(readData, 0, 0, kTexWidth, kTexHeight)) {
            const std::string error = "Texture::read failed";
            spdlog::error("  x {}: ERROR - {}", name, error);
            return {name, false, error};
        }

        // The returned buffer's length must match.
        if (readData.size() != expectedData.size()) {
            const std::string error = "Buffer length mismatch: expected " + std::to_string(expectedData.size()) +
                ", got " + std::to_string(readData.size());
            spdlog::error("  x {}: FAILED - {}", name, error);
            return {name, false, error};
        }

        // Compare.
        for (size_t i = 0; i < expectedData.size(); ++i) {
            if (expectedData[i] != readData[i]) {
                const std::string error = "Mismatch at index " + std::to_string(i) + ": expected " +
                    std::to_string(expectedData[i]) + ", got " + std::to_string(readData[i]);
                spdlog::error("  x {}: FAILED - {}", name, error);
                return {name, false, error};
            }
        }
        spdlog::info("  ok {}: PASSED", name);
        return {name, true, {}};
    }

    void runTests()
    {
        const std::string rule(60, '=');
        spdlog::info("{}", rule);
        spdlog::info("Running texture.read() tests on {}", backendName());
        spdlog::info("{}", rule);

        // Sequentially, one format at a time.
        std::vector<TestResult> passed;
        std::vector<TestResult> failed;
        for (const auto& formatInfo : kFormatsToTest) {
            TestResult result = testFormat(formatInfo);
            (result.passed ? passed : failed).push_back(std::move(result));
        }

        // Summary.
        spdlog::info("{}", rule);
        spdlog::info("Results: {}/{} passed", passed.size(), passed.size() + failed.size());

        std::string passedNames;
        for (const auto& result : passed) {
            passedNames += (passedNames.empty() ? "" : ", ") + result.name;
        }

        if (failed.empty()) {
            spdlog::info("ALL TESTS PASSED");
            _resultOverlay.setText("ALL TESTS PASSED");
            _resultOverlay.setBackground(Color(0.0f, 128.0f / 255.0f, 0.0f, 1.0f), 0.9f);
            _detailsOverlay.setText(std::to_string(passed.size()) + " formats tested successfully:\n" + passedNames);
        } else {
            spdlog::error("TESTS FAILED");
            std::string failedLines;
            for (const auto& result : failed) {
                spdlog::error("  - {}: {}", result.name, result.error);
                failedLines += (failedLines.empty() ? "" : "\n") + result.name + ": " + result.error;
            }
            _resultOverlay.setText("TESTS FAILED");
            _resultOverlay.setBackground(Color(200.0f / 255.0f, 0.0f, 0.0f, 1.0f), 0.9f);
            _detailsOverlay.setText("Failed formats:\n" + failedLines + "\n\nPassed: " + passedNames);
        }

        spdlog::info("{}", rule);
    }

    std::unique_ptr<Asset> _regular;
    std::unique_ptr<Asset> _bold;
    ScreenComponent* _screen = nullptr;
    Overlay _deviceInfo;
    Overlay _resultOverlay;
    Overlay _detailsOverlay;
};

VISUTWIN_EXAMPLE_MAIN(TextureReadExample)
