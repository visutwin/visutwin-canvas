// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream test/texture-copy (a hidden test example).
//
// Texture::copy between textures on the GPU, each copy checked by reading the destination back
// and comparing it byte for byte with what it should hold. Four rows, each a source on the left
// and its copy on the right (96 x 96 image elements, 24 apart, on a 960 x 720 screen blending
// width and height): the whole of the loaded logo texture (playcanvas.png); a procedural 64 x 64 texture
// of four coloured quadrants (red, green, blue, yellow); the second mip level of the same
// pattern with mipmaps, copied into level 0 of a 32 x 32 texture; and the top-left quarter of
// the quadrants copied to the bottom-right quarter of a grey texture, the grey kept around it.
// The device name and "left: source, right: Texture.copy() result" sit at the top, and the
// verdict with one line per row at the bottom, green when all pass, red otherwise; every
// result goes to the log too. The camera clears to (0.1, 0.1, 0.1).
//
// The copies and the reads run once, from create(), before the first frame: a read blocks,
// and Vulkan neither copies nor reads while a frame is recording.
//
// DEVIATIONS:
// - The HTML overlays are UI elements on a screen at one unit per window point, with square
//   corners, in Liberation Sans (metrically Arial; no monospace font ships).
// - A texture is filled through setLevelData and upload() where upstream locks and unlocks
//   it; the loaded texture's expected bytes are read back from it, as upstream does.
// - The images show their textures decoded from gamma, as every colour texture is here.
//   Upstream draws an image element's texture that is not flagged sRGB (these are not)
//   undecoded, so its thumbnails are gamma-encoded once more and read lighter (the loaded
//   texture's orange is (237, 161, 118) there, (218, 93, 47) here, its texel exactly).
//
#include <array>
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
#include "framework/components/screen/screenComponent.h"
#include "platform/graphics/constants.h"
#include "platform/graphics/texture.h"

using namespace visutwin::canvas;

namespace
{
    using Rgba = std::array<uint8_t, 4>;

    constexpr Rgba kRed{220, 40, 40, 255};
    constexpr Rgba kGreen{40, 200, 40, 255};
    constexpr Rgba kBlue{40, 80, 230, 255};
    constexpr Rgba kYellow{230, 220, 40, 255};
    constexpr Rgba kGrey{60, 60, 60, 255};

    // On-screen size of each texture, and the gap between them, in reference pixels.
    constexpr float kDisplay = 96.0f;
    constexpr float kGap = 24.0f;

    // A level filled with four quadrants: top left, top right, bottom left, bottom right.
    std::vector<uint8_t> quadrants(const uint32_t size)
    {
        const std::array<Rgba, 4> colors = {kRed, kGreen, kBlue, kYellow};
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
        const uint32_t half = size / 2;
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const Rgba& c = colors[(y < half ? 0 : 2) + (x < half ? 0 : 1)];
                std::copy(c.begin(), c.end(), pixels.begin() + (static_cast<size_t>(y) * size + x) * 4);
            }
        }
        return pixels;
    }

    std::vector<uint8_t> solid(const uint32_t size, const Rgba& color)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            std::copy(color.begin(), color.end(), pixels.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return pixels;
    }
}

class TextureCopyExample final : public ExampleApp
{
public:
    TextureCopyExample() : ExampleApp({.title = "Texture Copy"}) {}

protected:
    void configure(AppOptions& options) override
    {
        registerUi(options);
    }

    bool create() override
    {
        _logo = std::make_unique<Asset>("playcanvas", AssetType::TEXTURE, assetPath("textures/playcanvas.png"));
        _font = std::make_unique<Asset>("liberation-sans", AssetType::FONT, assetPath("fonts/liberation-sans.json"));
        Texture* loaded = _logo->resourceAs<Texture>();
        FontResource* font = _font->resourceAs<FontResource>();
        if (!loaded || !font) {
            spdlog::error("Failed to load textures/playcanvas.png or fonts/liberation-sans.json");
            return false;
        }

        auto* camera = createCamera(Vector3(0.0f, 0.0f, 0.0f));
        camera->findComponent<CameraComponent>()->camera()->setClearColor(Color(0.1f, 0.1f, 0.1f, 1.0f));

        // The screen the textures are shown on
        _screen = createScreen({.referenceResolution = Vector2(960.0f, 720.0f)});
        // The overlays, at one unit per window point
        _overlayScreen = createScreen({.scaleMode = ScreenScaleMode::None});

        _deviceInfo = createOverlay(Vector4(0.5f, 1.0f, 0.5f, 1.0f), Vector2(0.5f, 1.0f), Vector2(0.0f, -12.0f),
            font, 16, Vector2(14.0f, 6.0f));
        setOverlayText(_deviceInfo, std::string("Device: ") + backendName() +
            " - left: source, right: Texture.copy() result");
        _resultOverlay = createOverlay(Vector4(0.5f, 0.0f, 0.5f, 0.0f), Vector2(0.5f, 0.0f), Vector2(0.0f, 12.0f),
            font, 14, Vector2(16.0f, 8.0f));

        // ── The loaded texture, whole ──
        {
            auto dest = makeTexture("copy-loaded", loaded->width(), loaded->height(), false, loaded->format());
            std::vector<uint8_t> expected;
            loaded->read(expected);
            runRow("loaded texture, full copy", loaded, dest, {}, expected);
        }

        // ── A procedural texture, whole ──
        {
            constexpr uint32_t size = 64;
            auto source = makeTexture("quads-src", size, size, false);
            source->setLevelData(0, quadrants(size).data(), quadrants(size).size());
            source->upload();
            auto dest = makeTexture("quads-dst", size, size, false);
            std::vector<uint8_t> expected;
            source->read(expected);
            runRow("procedural, full copy", source.get(), dest, {}, expected);
            _textures.push_back(source);
        }

        // ── A source mip into the destination's level 0 ──
        {
            constexpr uint32_t size = 64;
            auto source = makeTexture("mips-src", size, size, true);
            source->setLevelData(0, quadrants(size).data(), quadrants(size).size());
            source->upload();
            auto dest = makeTexture("mips-dst", size >> 1, size >> 1, false);
            std::vector<uint8_t> expected;
            source->read(expected, 0, 0, 0, 0, 1);
            Texture::CopyOptions options;
            options.sourceMipLevel = 1;
            runRow("source mip 1 -> dest mip 0", source.get(), dest, options, expected);
            _textures.push_back(source);
        }

        // ── A sub-rectangle to an offset ──
        {
            constexpr uint32_t size = 64;
            constexpr uint32_t half = size / 2;
            auto source = makeTexture("subrect-src", size, size, false);
            source->setLevelData(0, quadrants(size).data(), quadrants(size).size());
            source->upload();
            auto dest = makeTexture("subrect-dst", size, size, false);
            const std::vector<uint8_t> grey = solid(size, kGrey);
            dest->setLevelData(0, grey.data(), grey.size());
            dest->upload();
            std::vector<uint8_t> expected = grey;
            for (uint32_t y = half; y < size; ++y) {
                for (uint32_t x = half; x < size; ++x) {
                    std::copy(kRed.begin(), kRed.end(), expected.begin() + (static_cast<size_t>(y) * size + x) * 4);
                }
            }
            Texture::CopyOptions options;
            options.width = half;
            options.height = half;
            options.destX = half;
            options.destY = half;
            runRow("sub-rect copy (offset)", source.get(), dest, options, expected);
            _textures.push_back(source);
        }

        // ── The verdict ──
        int passed = 0;
        std::string lines;
        for (const auto& [label, ok] : _results) {
            passed += ok ? 1 : 0;
            lines += std::string("\n") + (ok ? "+ " : "x ") + label;
        }
        const bool allPassed = passed == static_cast<int>(_results.size());
        setOverlayText(_resultOverlay, std::string(allPassed ? "ALL TESTS PASSED" : "TESTS FAILED") + " (" +
            std::to_string(passed) + "/" + std::to_string(_results.size()) + ")" + lines);
        _resultOverlay.text->setColor(allPassed ? Color(124.0f / 255.0f, 252.0f / 255.0f, 124.0f / 255.0f, 1.0f)
                                                : Color(1.0f, 107.0f / 255.0f, 107.0f / 255.0f, 1.0f));
        spdlog::info("Texture.copy: {}/{} passed on {}", passed, _results.size(), backendName());
        return true;
    }

private:
    struct Overlay
    {
        ElementComponent* panel = nullptr;
        ElementComponent* text = nullptr;
        Vector2 padding;
    };

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

    std::shared_ptr<Texture> makeTexture(const char* name, const uint32_t width, const uint32_t height,
        const bool mipmaps,
        const PixelFormat format = PixelFormat::PIXELFORMAT_RGBA8)
    {
        TextureOptions options;
        options.name = name;
        options.width = width;
        options.height = height;
        options.format = format;
        options.mipmaps = mipmaps;
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        auto texture = std::make_shared<Texture>(device().get(), options);
        texture->setAddressU(ADDRESS_CLAMP_TO_EDGE);
        texture->setAddressV(ADDRESS_CLAMP_TO_EDGE);
        texture->upload();
        return texture;
    }

    // One row: the source and the copy side by side, then the copy read back and compared.
    void runRow(const std::string& label, Texture* source, const std::shared_ptr<Texture>& dest,
        const Texture::CopyOptions& options, const std::vector<uint8_t>& expected)
    {
        const bool copied = dest->copy(*source, options);
        addImage(source, 0);
        addImage(dest.get(), 1);
        _rowY += kDisplay + kGap;
        _textures.push_back(dest);

        bool passed = copied;
        std::string detail;
        if (copied) {
            std::vector<uint8_t> actual;
            passed = dest->read(actual) && !actual.empty() && actual == expected;
            if (!passed) {
                detail = " (pixel mismatch)";
            }
        } else {
            detail = " (copy returned false)";
        }
        spdlog::info("{}: {}{}", passed ? "PASS" : "FAIL", label, detail);
        _results.emplace_back(label, passed);
    }

    // An image element showing a texture in a grid cell, its top left at (120 + column, -rowY).
    void addImage(Texture* texture, const int column)
    {
        auto* image = createElement(engine(), _screen->entity(), {.type = ElementType::Image, .texture = texture,
            .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f), .pivot = Vector2(0.0f, 1.0f), .width = kDisplay,
            .height = kDisplay});
        image->entity()->setLocalPosition(120.0f + static_cast<float>(column) * (kDisplay + kGap), -_rowY, 0.0f);
    }

    Overlay createOverlay(const Vector4& anchor, const Vector2& pivot, const Vector2& position, FontResource* font,
        const int fontSize, const Vector2& padding) const
    {
        Overlay overlay;
        overlay.padding = padding;
        overlay.panel = createElement(engine(), _overlayScreen->entity(), {.type = ElementType::Image,
            .color = Color(0.0f, 0.0f, 0.0f, 1.0f), .opacity = 0.7f, .anchor = anchor, .pivot = pivot});
        overlay.panel->entity()->setLocalPosition(position.x, position.y, 0.0f);
        overlay.text = createElement(engine(), overlay.panel->entity(), {.type = ElementType::Text,
            .color = Color(1.0f, 1.0f, 1.0f, 1.0f), .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
            .pivot = Vector2(0.5f, 0.5f), .font = font, .fontSize = fontSize,
            .horizontalAlign = ElementHorizontalAlign::Center});
        return overlay;
    }

    static void setOverlayText(const Overlay& overlay, const std::string& value)
    {
        overlay.text->setText(value);
        // The box follows the text, which takes its size at once.
        overlay.panel->setWidth(overlay.text->width() + 2.0f * overlay.padding.x);
        overlay.panel->setHeight(overlay.text->height() + 2.0f * overlay.padding.y);
    }

    std::unique_ptr<Asset> _logo;
    std::unique_ptr<Asset> _font;
    ScreenComponent* _screen = nullptr;
    ScreenComponent* _overlayScreen = nullptr;
    Overlay _deviceInfo;
    Overlay _resultOverlay;
    std::vector<std::shared_ptr<Texture>> _textures;
    std::vector<std::pair<std::string, bool>> _results;
    float _rowY = 70.0f;
};

VISUTWIN_EXAMPLE_MAIN(TextureCopyExample)
