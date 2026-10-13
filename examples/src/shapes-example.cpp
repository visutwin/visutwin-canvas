// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream graphics/shapes.
//
// Every built-in primitive (box, plane, cone, cylinder, sphere and capsule) in two rows of
// three, all sharing one default StandardMaterial, lit by an unshadowed directional light and
// an ambient of 0.2, with scene shadows switched off. The camera orbits the shapes. A UV test
// texture can be applied to the shared material: an 8x8 grid of 64-pixel cells whose hue
// changes along u and whose lightness alternates like a checkerboard, each cell labelled with
// its column and row, so the orientation and any stretching of each shape's UVs are easy to see.
//
// Keys standing in for upstream's controls panel:
//   T  texture on / off (off to begin with)
//
// DEVIATION: upstream draws the cell labels with the browser's bold sans-serif font. There is
// no text rasteriser here, so the labels are drawn with a small embedded 5x7 bitmap font of the
// digits and the comma, scaled by 2.
// DEVIATION: upstream gives the texture an anisotropy of 8; a texture here has no anisotropy
// setting of its own.
//
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../exampleApp.h"
#include "extras/script/cameraControls.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"
#include "platform/input/keyboard.h"
#include "scene/constants.h"
#include "scene/materials/standardMaterial.h"

using namespace visutwin::canvas;

namespace
{
    constexpr int kCells = 8;
    constexpr int kCellSize = 64;
    constexpr int kTextureSize = kCells * kCellSize;

    // Each glyph is 7 rows of a fixed width, 'X' for an inked pixel.
    struct Glyph
    {
        int width;
        std::array<const char*, 7> rows;
    };

    const std::array<Glyph, 8> kDigits{{
        {5, {".XXX.", "X...X", "X..XX", "X.X.X", "XX..X", "X...X", ".XXX."}},
        {5, {"..X..", ".XX..", "..X..", "..X..", "..X..", "..X..", ".XXX."}},
        {5, {".XXX.", "X...X", "....X", "...X.", "..X..", ".X...", "XXXXX"}},
        {5, {"XXXXX", "...X.", "..X..", "...X.", "....X", "X...X", ".XXX."}},
        {5, {"...X.", "..XX.", ".X.X.", "X..X.", "XXXXX", "...X.", "...X."}},
        {5, {"XXXXX", "X....", "XXXX.", "....X", "....X", "X...X", ".XXX."}},
        {5, {"..XX.", ".X...", "X....", "XXXX.", "X...X", "X...X", ".XXX."}},
        {5, {"XXXXX", "....X", "...X.", "..X..", ".X...", ".X...", ".X..."}},
    }};

    const Glyph kComma{2, {"..", "..", "..", "..", "XX", ".X", "X."}};

    constexpr int kGlyphScale = 2;
    constexpr int kGlyphSpacing = 1;

    // CSS hsl() to 8-bit RGB: hue in degrees, saturation and lightness in [0, 1].
    std::array<uint8_t, 3> hslToRgb(const float hue, const float saturation, const float lightness)
    {
        const float chroma = (1.0f - std::abs(2.0f * lightness - 1.0f)) * saturation;
        const float h = hue / 60.0f;
        const float x = chroma * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
        float r = 0.0f, g = 0.0f, b = 0.0f;
        if (h < 1.0f) {
            r = chroma; g = x;
        } else if (h < 2.0f) {
            r = x; g = chroma;
        } else if (h < 3.0f) {
            g = chroma; b = x;
        } else if (h < 4.0f) {
            g = x; b = chroma;
        } else if (h < 5.0f) {
            r = x; b = chroma;
        } else {
            r = chroma; b = x;
        }
        const float m = lightness - chroma * 0.5f;
        const auto toByte = [m](const float value) {
            return static_cast<uint8_t>(std::lround((value + m) * 255.0f));
        };
        return {toByte(r), toByte(g), toByte(b)};
    }

    // The UV grid, row 0 at the top as the canvas it stands in for.
    std::vector<uint8_t> paintUvGrid()
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(kTextureSize) * kTextureSize * 4);
        const auto setPixel = [&pixels](const int x, const int y, const std::array<uint8_t, 3>& rgb) {
            if (x < 0 || y < 0 || x >= kTextureSize || y >= kTextureSize) {
                return;
            }
            uint8_t* pixel = &pixels[(static_cast<size_t>(y) * kTextureSize + x) * 4];
            pixel[0] = rgb[0];
            pixel[1] = rgb[1];
            pixel[2] = rgb[2];
            pixel[3] = 255;
        };

        for (int column = 0; column < kCells; ++column) {
            for (int row = 0; row < kCells; ++row) {
                const auto fill = hslToRgb(static_cast<float>(column * 360) / kCells, 0.7f,
                    (column + row) % 2 ? 0.45f : 0.65f);
                for (int y = 0; y < kCellSize; ++y) {
                    for (int x = 0; x < kCellSize; ++x) {
                        setPixel(column * kCellSize + x, row * kCellSize + y, fill);
                    }
                }

                // The "column,row" label, centred in the cell, in black
                const std::array<const Glyph*, 3> label{&kDigits[column], &kComma, &kDigits[row]};
                int textWidth = -kGlyphSpacing;
                for (const Glyph* glyph : label) {
                    textWidth += glyph->width + kGlyphSpacing;
                }
                int penX = column * kCellSize + (kCellSize - textWidth * kGlyphScale) / 2;
                const int top = row * kCellSize + (kCellSize - 7 * kGlyphScale) / 2;
                for (const Glyph* glyph : label) {
                    for (int gy = 0; gy < 7; ++gy) {
                        for (int gx = 0; gx < glyph->width; ++gx) {
                            if (glyph->rows[gy][gx] != 'X') {
                                continue;
                            }
                            for (int sy = 0; sy < kGlyphScale; ++sy) {
                                for (int sx = 0; sx < kGlyphScale; ++sx) {
                                    setPixel(penX + gx * kGlyphScale + sx, top + gy * kGlyphScale + sy, {0, 0, 0});
                                }
                            }
                        }
                    }
                    penX += (glyph->width + kGlyphSpacing) * kGlyphScale;
                }
            }
        }
        return pixels;
    }
}

class ShapesExample final: public ExampleApp
{
public:
    ShapesExample(): ExampleApp({.title = "Shapes"}) {}

protected:
    bool create() override
    {
        scene()->setAmbientLight(0.2f, 0.2f, 0.2f);
        scene()->lighting().shadowsEnabled = false;

        // The UV test texture: mipmapped, its mips generated on upload
        TextureOptions options;
        options.name = "uv-grid";
        options.width = kTextureSize;
        options.height = kTextureSize;
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = true;
        _uvTexture = std::make_shared<Texture>(device().get(), options);
        const std::vector<uint8_t> pixels = paintUvGrid();
        _uvTexture->setLevelData(0, pixels.data(), pixels.size());
        _uvTexture->upload();

        // A single material shared by all shapes, so the texture toggle applies to all of them
        _material = std::make_shared<StandardMaterial>();

        // Every primitive shape type, in two rows of three
        const char* shapes[] = {"box", "plane", "cone", "cylinder", "sphere", "capsule"};
        int x = -1;
        int y = -1;
        for (const char* shape : shapes) {
            Entity* entity = createPrimitive(shape, _material.get(),
                Vector3(static_cast<float>(x) * 1.2f, static_cast<float>(y), 0.0f));
            entity->setName(shape);
            if (x++ == 1) {
                x = -1;
                y = 1;
            }
        }

        createDirectionalLight(Vector3(45.0f, 30.0f, 0.0f));

        // A camera orbiting the shapes
        auto* camera = createCamera(Vector3(0.0f, 0.0f, 5.0f));
        if (auto* component = camera->findComponent<CameraComponent>(); component && component->camera()) {
            component->camera()->setClearColor(Color(0.4f, 0.45f, 0.5f, 1.0f));
        }
        if (auto* controls = addOrbitControls(camera, Vector3(0.0f, 0.0f, 0.0f))) {
            controls->setEnableFly(false);
            controls->storeResetState();
        }

        applyTexture();
        spdlog::info("Keys: T texture on / off");
        return true;
    }

    void update(float /*dt*/) override
    {
        if (const auto* keyboard = engine()->keyboard(); keyboard && keyboard->wasPressed(Key::T)) {
            _textured = !_textured;
            applyTexture();
        }
    }

private:
    // The diffuse map is the UV grid while the texture toggle is on
    void applyTexture() const
    {
        _material->setDiffuseMap(_textured ? _uvTexture.get() : nullptr);
    }

    std::shared_ptr<Texture> _uvTexture;
    std::shared_ptr<StandardMaterial> _material;
    bool _textured = false;
};

VISUTWIN_EXAMPLE_MAIN(ShapesExample)
