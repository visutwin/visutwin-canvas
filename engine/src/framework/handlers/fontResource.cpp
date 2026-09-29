// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "fontResource.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <stb_image.h>
#include <spdlog/spdlog.h>

#include "framework/assets/stbImageFlip.h"
// TexHint, for tagging the font atlas into the asset VRAM bucket. Reachable
// transitively through graphicsDevice.h -> texture.h, but named directly here.
#include "platform/graphics/constants.h"
#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    namespace
    {
        bool parseNumberField(const std::string& block, const std::string& key, float& out)
        {
            const std::string marker = "\"" + key + "\":";
            const size_t p = block.find(marker);
            if (p == std::string::npos) {
                return false;
            }
            size_t i = p + marker.size();
            while (i < block.size() && std::isspace(static_cast<unsigned char>(block[i]))) {
                i++;
            }
            size_t j = i;
            while (j < block.size()) {
                const char c = block[j];
                if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                    j++;
                } else {
                    break;
                }
            }
            if (j <= i) {
                return false;
            }
            out = std::stof(block.substr(i, j - i));
            return true;
        }

        bool parseIntField(const std::string& block, const std::string& key, int& out)
        {
            float v = 0.0f;
            if (!parseNumberField(block, key, v)) {
                return false;
            }
            out = static_cast<int>(std::lround(v));
            return true;
        }

        std::optional<std::string> readTextFile(const std::string& path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open()) {
                return std::nullopt;
            }
            std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            return data;
        }

        std::string replaceExtensionWithPng(const std::string& path)
        {
            const size_t dot = path.find_last_of('.');
            if (dot == std::string::npos) {
                return path + ".png";
            }
            return path.substr(0, dot) + ".png";
        }

        std::optional<size_t> findMatchingBrace(const std::string& text, const size_t openPos)
        {
            if (openPos >= text.size() || text[openPos] != '{') {
                return std::nullopt;
            }
            int depth = 0;
            bool inString = false;
            bool escaped = false;
            for (size_t i = openPos; i < text.size(); ++i) {
                const char c = text[i];
                if (inString) {
                    if (escaped) {
                        escaped = false;
                        continue;
                    }
                    if (c == '\\') {
                        escaped = true;
                        continue;
                    }
                    if (c == '"') {
                        inString = false;
                    }
                    continue;
                }

                if (c == '"') {
                    inString = true;
                    continue;
                }
                if (c == '{') {
                    depth++;
                } else if (c == '}') {
                    depth--;
                    if (depth == 0) {
                        return i;
                    }
                }
            }
            return std::nullopt;
        }

        bool parseStrictInt(const std::string& token, int& out)
        {
            if (token.empty()) {
                return false;
            }
            for (const char c : token) {
                if (c < '0' || c > '9') {
                    return false;
                }
            }
            try {
                out = std::stoi(token);
                return true;
            } catch (...) {
                return false;
            }
        }

        /// One atlas page. An MSDF page keeps its distance field untouched and is sampled
        /// BILINEARLY — the field is what makes an edge sharp at any scale. A bitmap page
        /// gets its coverage in alpha (lifted from RGB when the alpha is flat) and keeps
        /// nearest filtering.
        Texture* loadFontPage(const std::string& path, const bool msdf, const std::shared_ptr<GraphicsDevice>& device)
        {
            int w = 0;
            int h = 0;
            int channels = 0;
            // Glyph rects are top-left origin, so the atlas must NOT be flipped. Clearing
            // only stb's global flag here was overridden by the GLB parser's thread-local
            // one, and a font loaded after any GLB drew every glyph upside down.
            stbi_uc* pixels = nullptr;
            {
                const StbVerticalFlipScope flipScope(false);
                pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
            }
            if (!pixels || w <= 0 || h <= 0) {
                if (pixels) {
                    stbi_image_free(pixels);
                }
                return nullptr;
            }
            const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
            if (!msdf) {
                uint8_t minA = 255;
                uint8_t maxA = 0;
                for (size_t i = 0; i < pixelCount; ++i) {
                    minA = std::min(minA, pixels[i * 4u + 3u]);
                    maxA = std::max(maxA, pixels[i * 4u + 3u]);
                }
                if (minA == maxA) {
                    for (size_t i = 0; i < pixelCount; ++i) {
                        const uint8_t cov = std::max(pixels[i * 4u + 0u], std::max(pixels[i * 4u + 1u], pixels[i * 4u + 2u]));
                        pixels[i * 4u + 0u] = 255;
                        pixels[i * 4u + 1u] = 255;
                        pixels[i * 4u + 2u] = 255;
                        pixels[i * 4u + 3u] = cov;
                    }
                }
            }

            TextureOptions options;
            options.width = static_cast<uint32_t>(w);
            options.height = static_cast<uint32_t>(h);
            options.format = PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = false;
            options.minFilter = msdf ? FilterMode::FILTER_LINEAR : FilterMode::FILTER_NEAREST;
            options.magFilter = msdf ? FilterMode::FILTER_LINEAR : FilterMode::FILTER_NEAREST;
            options.numLevels = 1;
            options.name = "font-atlas";
            options.profilerHint = TexHint::TEXHINT_ASSET;
            auto* texture = new Texture(device.get(), options);
            // Distance values, not colour: never sRGB-decoded.
            texture->setEncoding(TextureEncoding::Default);
            texture->setLevelData(0, reinterpret_cast<const uint8_t*>(pixels), pixelCount * 4u);
            texture->upload();
            stbi_image_free(pixels);
            return texture;
        }
    }

    // DEVIATION: parser is a lightweight JSON-string scanner for bitmap-font schema.
    std::optional<FontResource*> loadBitmapFontResource(const std::string& jsonPath,
        const std::shared_ptr<GraphicsDevice>& graphicsDevice)
    {
        if (!graphicsDevice) {
            return std::nullopt;
        }

        const auto jsonText = readTextFile(jsonPath);
        if (!jsonText.has_value()) {
            return std::nullopt;
        }
        const std::string& text = *jsonText;

        auto* font = new FontResource();

        // info.maps: one { width, height } per atlas page.
        std::vector<std::pair<int, int>> pageSizes;
        {
            const std::string mapsMarker = "\"maps\":[";
            const size_t mapsPos = text.find(mapsMarker);
            if (mapsPos != std::string::npos) {
                const size_t listEnd = text.find(']', mapsPos);
                size_t p = mapsPos + mapsMarker.size();
                while (listEnd != std::string::npos && p < listEnd) {
                    const size_t mapStart = text.find('{', p);
                    if (mapStart == std::string::npos || mapStart > listEnd) {
                        break;
                    }
                    const size_t mapEnd = text.find('}', mapStart);
                    if (mapEnd == std::string::npos) {
                        break;
                    }
                    const std::string mapBlock = text.substr(mapStart, mapEnd - mapStart + 1);
                    int width = 0;
                    int height = 0;
                    parseIntField(mapBlock, "width", width);
                    parseIntField(mapBlock, "height", height);
                    pageSizes.emplace_back(width, height);
                    p = mapEnd + 1;
                }
            }
        }
        if (!pageSizes.empty()) {
            font->atlasWidth = pageSizes[0].first;
            font->atlasHeight = pageSizes[0].second;
        }
        parseNumberField(text, "intensity", font->intensity);

        {
            const std::string charsMarker = "\"chars\":{";
            const size_t charsPos = text.find(charsMarker);
            if (charsPos != std::string::npos) {
                const size_t objStart = text.find('{', charsPos);
                const auto objEndOpt = objStart != std::string::npos ? findMatchingBrace(text, objStart) : std::nullopt;
                if (!objEndOpt.has_value() || *objEndOpt <= objStart) {
                    delete font;
                    return std::nullopt;
                }
                const std::string charsBlock = text.substr(objStart + 1, *objEndOpt - objStart - 1);

                size_t p = 0;
                while (true) {
                    const size_t keyStart = charsBlock.find('"', p);
                    if (keyStart == std::string::npos) break;
                    const size_t keyEnd = charsBlock.find('"', keyStart + 1);
                    if (keyEnd == std::string::npos) break;
                    int charId = 0;
                    if (!parseStrictInt(charsBlock.substr(keyStart + 1, keyEnd - keyStart - 1), charId)) {
                        p = keyEnd + 1;
                        continue;
                    }
                    const size_t glyphObjStart = charsBlock.find('{', keyEnd);
                    if (glyphObjStart == std::string::npos) break;
                    const auto glyphObjEndOpt = findMatchingBrace(charsBlock, glyphObjStart);
                    if (!glyphObjEndOpt.has_value() || *glyphObjEndOpt <= glyphObjStart) break;

                    const std::string block = charsBlock.substr(glyphObjStart, *glyphObjEndOpt - glyphObjStart + 1);
                    FontGlyph glyph{};
                    glyph.id = charId;
                    parseNumberField(block, "x", glyph.x);
                    parseNumberField(block, "y", glyph.y);
                    parseNumberField(block, "width", glyph.width);
                    parseNumberField(block, "height", glyph.height);
                    parseNumberField(block, "xadvance", glyph.xadvance);
                    parseNumberField(block, "xoffset", glyph.xoffset);
                    parseNumberField(block, "yoffset", glyph.yoffset);
                    parseIntField(block, "map", glyph.page);
                    // Upstream _getPxRange: scale x range of the first glyph that has one.
                    if (float range = 0.0f; !font->msdf && parseNumberField(block, "range", range) && range > 0.0f) {
                        float scale = 1.0f;
                        parseNumberField(block, "scale", scale);
                        font->pxRange = (scale > 0.0f ? scale : 1.0f) * range;
                        font->msdf = true;
                    }
                    font->glyphs[glyph.id] = glyph;

                    font->lineHeight = std::max(font->lineHeight, glyph.height);
                    p = *glyphObjEndOpt + 1;
                }
            }
        }

        {
            const std::string kerningMarker = "\"kerning\":{";
            const size_t kernPos = text.find(kerningMarker);
            if (kernPos != std::string::npos) {
                const size_t objStart = text.find('{', kernPos);
                const auto objEndOpt = objStart != std::string::npos ? findMatchingBrace(text, objStart) : std::nullopt;
                if (objEndOpt.has_value() && *objEndOpt > objStart) {
                    const std::string block = text.substr(objStart, *objEndOpt - objStart + 1);
                        size_t p = 0;
                        while (true) {
                            const size_t k1 = block.find('"', p);
                            if (k1 == std::string::npos) break;
                            const size_t k2 = block.find('"', k1 + 1);
                            if (k2 == std::string::npos) break;
                            int left = 0;
                            if (!parseStrictInt(block.substr(k1 + 1, k2 - k1 - 1), left)) {
                                p = k2 + 1;
                                continue;
                            }
                            const size_t subObjStart = block.find('{', k2);
                            if (subObjStart == std::string::npos) break;
                            const auto subObjEndOpt = findMatchingBrace(block, subObjStart);
                            if (!subObjEndOpt.has_value()) break;
                            const size_t subObjEnd = *subObjEndOpt;
                            const std::string sub = block.substr(subObjStart, subObjEnd - subObjStart + 1);

                            size_t q = 0;
                            while (true) {
                                const size_t r1 = sub.find('"', q);
                                if (r1 == std::string::npos) break;
                                const size_t r2 = sub.find('"', r1 + 1);
                                if (r2 == std::string::npos) break;
                                int right = 0;
                                if (!parseStrictInt(sub.substr(r1 + 1, r2 - r1 - 1), right)) {
                                    q = r2 + 1;
                                    continue;
                                }
                                float value = 0.0f;
                                const std::string numKey = "\"" + std::to_string(right) + "\":";
                                const size_t vPos = sub.find(numKey, r2);
                                if (vPos != std::string::npos) {
                                    const size_t nStart = vPos + numKey.size();
                                    size_t nEnd = nStart;
                                    while (nEnd < sub.size()) {
                                        const char c = sub[nEnd];
                                        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
                                            nEnd++;
                                        } else {
                                            break;
                                        }
                                    }
                                    if (nEnd > nStart) {
                                        value = std::stof(sub.substr(nStart, nEnd - nStart));
                                        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(left)) << 32u) |
                                            static_cast<uint32_t>(right);
                                        font->kerning[key] = value;
                                    }
                                }
                                q = r2 + 1;
                            }
                            p = subObjEnd + 1;
                        }
                    }
                }
            }

        // One image per page: <name>.png, <name>1.png, <name>2.png, ... (upstream's font
        // handler names them the same way).
        const std::string basePath = replaceExtensionWithPng(jsonPath);
        const std::string stem = basePath.substr(0, basePath.size() - 4);
        const size_t pageCount = std::max<size_t>(pageSizes.size(), 1);
        for (size_t page = 0; page < pageCount; ++page) {
            const std::string pagePath = page == 0 ? basePath : stem + std::to_string(page) + ".png";
            Texture* texture = loadFontPage(pagePath, font->msdf, graphicsDevice);
            if (!texture) {
                spdlog::error("Font '{}': atlas page {} ('{}') failed to load", jsonPath, page, pagePath);
                delete font;
                return std::nullopt;
            }
            font->pages.push_back(texture);
        }
        font->texture = font->pages[0];
        if (font->atlasWidth <= 0) font->atlasWidth = static_cast<int>(font->texture->width());
        if (font->atlasHeight <= 0) font->atlasHeight = static_cast<int>(font->texture->height());
        if (font->lineHeight <= 0.0f) font->lineHeight = 64.0f;
        spdlog::info("Loaded {} font '{}': {} page(s), {}x{}, glyphs={}, kerning={}, pxrange={}",
            font->msdf ? "MSDF" : "bitmap", jsonPath, font->pages.size(), font->atlasWidth, font->atlasHeight,
            font->glyphs.size(), font->kerning.size(), font->pxRange);
        return font;
    }
}
