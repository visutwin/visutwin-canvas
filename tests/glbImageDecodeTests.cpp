// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A glTF's images are decoded by GlbParser::prepareFromModel, all at once, one per
// thread. tinygltf's image callback (GlbParser::loadImageData) decodes nothing: it keeps
// each image's encoded bytes, marked `as_is`. Decoding in the callback decodes a model's
// images one after another on the loading thread, which is three quarters of what a
// textured model takes to load.
//
// What must hold is that every image comes out exactly as the one-at-a-time path made
// it — RGBA8 whatever the file's channel count, flipped vertically (the parser flips V
// into the vertices, so its textures are stored bottom row first) — and that images
// decoded side by side do not end up in each other's slots.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <stb_image_write.h>
#include <tiny_gltf.h>

#include "framework/parsers/glbParser.h"
#include "platform/graphics/constants.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        std::cout << (condition ? "  ok   " : "  FAIL ") << what << '\n';
        if (!condition) {
            ++failures;
        }
    }

    void appendToVector(void* context, void* data, const int size)
    {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    }

    struct Source
    {
        int width = 0;
        int height = 0;
        int components = 0;
        std::vector<uint8_t> pixels;   // top row first, `components` bytes a pixel
    };

    // Every pixel different from its neighbours, and different per image (`seed`).
    Source makeSource(const int width, const int height, const int components, const int seed)
    {
        Source source{width, height, components, {}};
        source.pixels.resize(static_cast<size_t>(width) * height * components);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < components; ++c) {
                    source.pixels[(static_cast<size_t>(y) * width + x) * components + c] =
                        static_cast<uint8_t>((seed * 37 + y * 29 + x * 11 + c * 53) & 0xFF);
                }
            }
        }
        return source;
    }

    std::vector<uint8_t> encodePng(const Source& source)
    {
        std::vector<uint8_t> png;
        stbi_write_png_to_func(appendToVector, &png, source.width, source.height, source.components,
            source.pixels.data(), source.width * source.components);
        return png;
    }

    // What the parser stores for `source`: RGBA8, bottom row first.
    std::vector<uint8_t> expectedRgba(const Source& source)
    {
        std::vector<uint8_t> rgba(static_cast<size_t>(source.width) * source.height * 4);
        for (int y = 0; y < source.height; ++y) {
            const int sourceRow = source.height - 1 - y;
            for (int x = 0; x < source.width; ++x) {
                const uint8_t* src = &source.pixels[(static_cast<size_t>(sourceRow) * source.width + x) * source.components];
                uint8_t* dst = &rgba[(static_cast<size_t>(y) * source.width + x) * 4];
                switch (source.components) {
                case 1: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255; break;
                case 2: dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1]; break;
                case 3: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; break;
                default: dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; break;
                }
            }
        }
        return rgba;
    }

    // An image as tinygltf hands it to the parser after its image callback ran.
    tinygltf::Image imageThroughCallback(const std::vector<uint8_t>& encoded, const int index)
    {
        tinygltf::Image image;
        image.name = "image" + std::to_string(index);
        std::string error;
        std::string warning;
        const bool ok = GlbParser::loadImageData(&image, index, &error, &warning, 0, 0,
            encoded.data(), static_cast<int>(encoded.size()), nullptr);
        if (!ok) {
            image.image.clear();
        }
        return image;
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "the callback keeps the bytes and decodes nothing\n";
    {
        const auto png = encodePng(makeSource(5, 3, 4, 1));
        const tinygltf::Image image = imageThroughCallback(png, 0);
        check(image.as_is, "the image is marked as_is");
        check(image.image == png, "and holds the file's bytes unchanged");
        check(image.width == 0 && image.height == 0, "with no decoded size yet");
    }

    std::cout << "\nevery channel count comes out RGBA8, bottom row first\n";
    {
        const std::vector<Source> sources = {
            makeSource(5, 3, 4, 1), makeSource(4, 4, 3, 2), makeSource(2, 6, 1, 3), makeSource(3, 5, 2, 4)};
        tinygltf::Model model;
        for (size_t i = 0; i < sources.size(); ++i) {
            model.images.push_back(imageThroughCallback(encodePng(sources[i]), static_cast<int>(i)));
        }
        const PreparedGlbData prepared = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, "test");
        check(prepared.images.size() == sources.size(), "one prepared image per glTF image");
        const char* names[] = {"RGBA", "RGB", "grey", "grey + alpha"};
        for (size_t i = 0; i < sources.size() && i < prepared.images.size(); ++i) {
            const auto& image = prepared.images[i];
            check(image.valid && !image.isCompressed && image.width == sources[i].width &&
                  image.height == sources[i].height, std::string(names[i]) + ": decoded at its own size");
            check(image.rgbaPixels == expectedRgba(sources[i]), std::string(names[i]) + ": the expected pixels");
        }
    }

    std::cout << "\nmany images decoded side by side stay in their own slots\n";
    {
        constexpr int kCount = 48;
        std::vector<Source> sources;
        tinygltf::Model model;
        for (int i = 0; i < kCount; ++i) {
            sources.push_back(makeSource(8 + i % 7, 5 + i % 5, 1 + i % 4, 100 + i));
            model.images.push_back(imageThroughCallback(encodePng(sources.back()), i));
        }
        const PreparedGlbData prepared = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, "test");
        int matching = 0;
        for (int i = 0; i < kCount && i < static_cast<int>(prepared.images.size()); ++i) {
            const auto& image = prepared.images[static_cast<size_t>(i)];
            if (image.valid && image.width == sources[static_cast<size_t>(i)].width &&
                image.height == sources[static_cast<size_t>(i)].height &&
                image.rgbaPixels == expectedRgba(sources[static_cast<size_t>(i)])) {
                ++matching;
            }
        }
        check(matching == kCount, std::to_string(matching) + " of " + std::to_string(kCount) +
            " images are their own source's pixels");
        const PreparedGlbData again = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, "test");
        bool same = again.images.size() == prepared.images.size();
        for (size_t i = 0; same && i < again.images.size(); ++i) {
            same = again.images[i].rgbaPixels == prepared.images[i].rgbaPixels;
        }
        check(same, "preparing the same model again gives the same images (the encoded bytes are kept)");
    }

    std::cout << "\nimages that are not files to decode\n";
    {
        tinygltf::Model model;
        // Bytes no decoder reads.
        model.images.push_back(imageThroughCallback({'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'}, 0));
        // Pixels someone else decoded: a model built in memory, top row first as given.
        tinygltf::Image decoded;
        decoded.width = 2;
        decoded.height = 2;
        decoded.component = 3;
        decoded.bits = 8;
        decoded.pixel_type = TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE;
        decoded.image = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
        model.images.push_back(decoded);

        const PreparedGlbData prepared = GlbParser::prepareFromModel(model, PixelFormat::PIXELFORMAT_RGBA8, "test");
        const auto& placeholder = prepared.images[0];
        check(placeholder.valid && placeholder.width == 1 && placeholder.height == 1 &&
              placeholder.rgbaPixels == std::vector<uint8_t>({255, 0, 255, 255}),
            "an undecodable image becomes a 1x1 magenta placeholder, so the model still loads");
        const auto& passedThrough = prepared.images[1];
        check(passedThrough.valid && passedThrough.width == 2 && passedThrough.height == 2 &&
              passedThrough.rgbaPixels == std::vector<uint8_t>({10, 20, 30, 255, 40, 50, 60, 255,
                                                               70, 80, 90, 255, 100, 110, 120, 255}),
            "already-decoded pixels are widened to RGBA and otherwise left as they are");
    }

    std::cout << (failures == 0 ? "\nAll glTF image decode tests passed\n" : "\nglTF image decode tests FAILED\n");
    return failures == 0 ? 0 : 1;
}
