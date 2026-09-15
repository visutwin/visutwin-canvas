// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Image orientation across loaders, pinned without a GPU.
//
// stb_image has a global and a thread-local vertical-flip flag, and once the
// thread-local one is set on a thread it overrides the global one there for good.
// The GLB parser set the thread-local flag to flip; the bitmap-font loader cleared
// only the global one, so a font loaded after any GLB drew every glyph upside down.
// Nothing in a render says WHICH loader ran first, so the order is checked here: an
// asymmetric atlas goes through the real font path after a GLB-style flipped decode
// and its first row must still be the image's top row.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

#include "framework/assets/stbImageFlip.h"
#include "framework/handlers/fontResource.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const std::string& what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what.c_str());
            ++failures;
        }
    }

    /// Creates nothing; enough for the font loader to build its Texture.
    class StubDevice final : public GraphicsDevice
    {
    public:
        void draw(const Primitive&, const std::shared_ptr<IndexBuffer>&, int, int, bool, bool) override {}
        void startRenderPass(RenderPass*) override {}
        void endRenderPass(RenderPass*) override {}
        std::unique_ptr<gpu::HardwareTexture> createGPUTexture(Texture*) override { return nullptr; }
        std::shared_ptr<VertexBuffer> createVertexBuffer(const std::shared_ptr<VertexFormat>&, int,
            const VertexBufferOptions&) override { return nullptr; }
        std::shared_ptr<IndexBuffer> createIndexBuffer(IndexFormat, int, const std::vector<uint8_t>&) override
        {
            return nullptr;
        }
        void setResolution(int, int) override {}
        std::pair<int, int> size() const override { return {0, 0}; }
        std::shared_ptr<RenderTarget> createRenderTarget(const RenderTargetOptions&) override { return nullptr; }
    };

    constexpr int kSize = 4;
    constexpr uint8_t kTopAlpha = 255;
    constexpr uint8_t kBottomAlpha = 40;

    /// 4x4 RGBA: the top two rows opaque, the bottom two faint. Alpha is not flat,
    /// so the font loader keeps it as it is.
    std::vector<uint8_t> asymmetricPixels()
    {
        std::vector<uint8_t> pixels(kSize * kSize * 4);
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const size_t i = static_cast<size_t>(y * kSize + x) * 4u;
                pixels[i] = 255;
                pixels[i + 1] = 255;
                pixels[i + 2] = 255;
                pixels[i + 3] = y < kSize / 2 ? kTopAlpha : kBottomAlpha;
            }
        }
        return pixels;
    }

    void appendToVector(void* context, void* data, const int size)
    {
        auto* out = static_cast<std::vector<uint8_t>*>(context);
        const auto* bytes = static_cast<uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
    }

    /// Top-row alpha of a decoded RGBA image.
    uint8_t topAlpha(const uint8_t* rgba) { return rgba[3]; }

    /// What the GLB parser does to an embedded image: decode it flipped.
    void decodeLikeGlb(const std::vector<uint8_t>& png, const bool throughScope)
    {
        int w = 0, h = 0, c = 0;
        stbi_uc* decoded = nullptr;
        if (throughScope) {
            const StbVerticalFlipScope flipScope(true);
            decoded = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &c, 4);
        } else {
            // The pre-fix parser: set the thread-local flag and leave it set.
            stbi_set_flip_vertically_on_load_thread(1);
            decoded = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &c, 4);
        }
        check(decoded != nullptr, "GLB-style decode succeeds");
        if (decoded) {
            check(topAlpha(decoded) == kBottomAlpha, "GLB-style decode is flipped");
            stbi_image_free(decoded);
        }
    }

    void checkFontAtlasUpright(const std::string& jsonPath, const std::shared_ptr<GraphicsDevice>& device,
        const std::string& label)
    {
        const auto font = loadBitmapFontResource(jsonPath, device);
        check(font.has_value() && *font && (*font)->texture, label + ": font loads");
        if (!font.has_value() || !*font || !(*font)->texture) {
            return;
        }
        const auto* level = static_cast<const uint8_t*>((*font)->texture->getLevel(0));
        check(level != nullptr, label + ": atlas has level data");
        if (level) {
            check(level[3] == kTopAlpha, label + ": atlas row 0 is the image's TOP row");
            check(level[(kSize * kSize - 1) * 4 + 3] == kBottomAlpha, label + ": atlas last row is the BOTTOM row");
        }
        delete *font;
    }
}

int main()
{
    const auto dir = std::filesystem::temp_directory_path() / "visutwin-stb-flip-tests";
    std::filesystem::create_directories(dir);

    const auto pixels = asymmetricPixels();
    std::vector<uint8_t> png;
    stbi_write_png_to_func(appendToVector, &png, kSize, kSize, 4, pixels.data(), kSize * 4);
    check(!png.empty(), "PNG encodes");

    const auto pngPath = dir / "atlas.png";
    {
        std::ofstream out(pngPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
    const auto jsonPath = dir / "atlas.json";
    {
        std::ofstream out(jsonPath);
        out << R"({"version":2,"info":{"face":"test","maps":[{"width":4,"height":4}]},)"
            << R"("chars":{"65":{"id":65,"letter":"A","x":0,"y":0,"width":4,"height":4,"xadvance":4,"xoffset":0,"yoffset":0}},)"
            << R"("kerning":{}})";
    }

    const auto device = std::make_shared<StubDevice>();

    // The scope sets and restores, innermost first.
    {
        check(!StbVerticalFlipScope::current(), "no flip outside any scope");
        {
            const StbVerticalFlipScope outer(true);
            check(StbVerticalFlipScope::current(), "outer scope flips");
            {
                const StbVerticalFlipScope inner(false);
                check(!StbVerticalFlipScope::current(), "inner scope does not flip");
            }
            check(StbVerticalFlipScope::current(), "outer flip restored after inner scope");
        }
        check(!StbVerticalFlipScope::current(), "default restored after outer scope");
    }

    // A GLB decode through the scope does not leak its flip into a bare decode.
    {
        decodeLikeGlb(png, true);
        int w = 0, h = 0, c = 0;
        stbi_uc* decoded = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &c, 4);
        check(decoded && topAlpha(decoded) == kTopAlpha, "a bare decode after a scoped GLB decode is upright");
        if (decoded) stbi_image_free(decoded);
    }

    // The reported defect: a font loaded after a GLB, with the flag LEFT flipped on
    // this thread (and the global flag cleared, which cannot override it). The font
    // loader must set its own orientation through the thread-local flag.
    {
        stbi_set_flip_vertically_on_load(0);
        decodeLikeGlb(png, false);

        // The trap itself: what the font loader used to do. Clearing the GLOBAL flag
        // is ignored once the thread-local one is set, and the decode comes out flipped.
        {
            stbi_set_flip_vertically_on_load(0);
            int w = 0, h = 0, c = 0;
            stbi_uc* decoded = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &c, 4);
            check(decoded && topAlpha(decoded) == kBottomAlpha,
                "stb trap reproduced: clearing the global flag does not override a set thread-local flag");
            if (decoded) stbi_image_free(decoded);
        }

        checkFontAtlasUpright(jsonPath.string(), device, "after a leaked thread-local flip");
    }

    // And after the parser's current, scoped decode.
    {
        decodeLikeGlb(png, true);
        checkFontAtlasUpright(jsonPath.string(), device, "after a scoped GLB decode");
    }

    std::filesystem::remove_all(dir);

    if (failures == 0) {
        std::printf("stb image flip tests passed\n");
        return 0;
    }
    std::printf("%d failure(s)\n", failures);
    return 1;
}
