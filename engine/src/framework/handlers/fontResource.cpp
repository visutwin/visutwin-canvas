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

        float median3(const float a, const float b, const float c)
        {
            return std::max(std::min(a, b), std::min(std::max(a, b), c));
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

        {
            const std::string mapMarker = "\"maps\":[{";
            const size_t mapPos = text.find(mapMarker);
            if (mapPos != std::string::npos) {
                const size_t mapStart = text.find('{', mapPos);
                const size_t mapEnd = text.find('}', mapStart);
                if (mapStart != std::string::npos && mapEnd != std::string::npos && mapEnd > mapStart) {
                    const std::string mapBlock = text.substr(mapStart, mapEnd - mapStart + 1);
                    parseIntField(mapBlock, "width", font->atlasWidth);
                    parseIntField(mapBlock, "height", font->atlasHeight);
                }
            }
        }

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

        const std::string atlasPath = replaceExtensionWithPng(jsonPath);
        int w = 0;
        int h = 0;
        int channels = 0;
        stbi_set_flip_vertically_on_load(false);
        stbi_uc* pixels = stbi_load(atlasPath.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels || w <= 0 || h <= 0) {
            delete font;
            if (pixels) {
                stbi_image_free(pixels);
            }
            return std::nullopt;
        }

        const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
        const bool likelySdfMsdf = text.find("\"range\":") != std::string::npos;

        // Current renderer path is bitmap-style. If the font looks like SDF/MSDF
        // (upstream courier does), pre-bake distance field into alpha coverage.
        if (likelySdfMsdf) {
            size_t midCountPos = 0;
            size_t midCountNeg = 0;
            std::vector<uint8_t> alphaPos(pixelCount);
            std::vector<uint8_t> alphaNeg(pixelCount);
            for (size_t i = 0; i < pixelCount; ++i) {
                const float r = static_cast<float>(pixels[i * 4u + 0u]) / 255.0f;
                const float g = static_cast<float>(pixels[i * 4u + 1u]) / 255.0f;
                const float b = static_cast<float>(pixels[i * 4u + 2u]) / 255.0f;
                const float sd = median3(r, g, b) - 0.5f;
                const float covPos = std::clamp(sd * 8.0f + 0.5f, 0.0f, 1.0f);
                const float covNeg = std::clamp((-sd) * 8.0f + 0.5f, 0.0f, 1.0f);
                const uint8_t aPos = static_cast<uint8_t>(std::lround(covPos * 255.0f));
                const uint8_t aNeg = static_cast<uint8_t>(std::lround(covNeg * 255.0f));
                alphaPos[i] = aPos;
                alphaNeg[i] = aNeg;
                if (aPos > 10 && aPos < 245) {
                    midCountPos++;
                }
                if (aNeg > 10 && aNeg < 245) {
                    midCountNeg++;
                }
            }
            const bool useNeg = midCountNeg > midCountPos;
            for (size_t i = 0; i < pixelCount; ++i) {
                pixels[i * 4u + 0u] = 255;
                pixels[i * 4u + 1u] = 255;
                pixels[i * 4u + 2u] = 255;
                pixels[i * 4u + 3u] = useNeg ? alphaNeg[i] : alphaPos[i];
            }
            spdlog::info(
                "Font atlas '{}' detected as SDF/MSDF; baked {} signed-distance coverage to alpha (mid-pos={}, mid-neg={}).",
                atlasPath,
                useNeg ? "negative" : "positive",
                midCountPos,
                midCountNeg
            );
        }

        // Fallback: some atlases still have flat alpha; lift coverage from RGB.
        uint8_t minA = 255;
        uint8_t maxA = 0;
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint8_t a = pixels[i * 4u + 3u];
            minA = std::min(minA, a);
            maxA = std::max(maxA, a);
        }
        if (minA == maxA) {
            for (size_t i = 0; i < pixelCount; ++i) {
                const uint8_t r = pixels[i * 4u + 0u];
                const uint8_t g = pixels[i * 4u + 1u];
                const uint8_t b = pixels[i * 4u + 2u];
                const uint8_t cov = std::max(r, std::max(g, b));
                pixels[i * 4u + 0u] = 255;
                pixels[i * 4u + 1u] = 255;
                pixels[i * 4u + 2u] = 255;
                pixels[i * 4u + 3u] = cov;
            }
            spdlog::info("Font atlas '{}' had flat alpha; applied RGB->alpha bitmap fallback.", atlasPath);
        }

        uint8_t outMinA = 255;
        uint8_t outMaxA = 0;
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint8_t a = pixels[i * 4u + 3u];
            outMinA = std::min(outMinA, a);
            outMaxA = std::max(outMaxA, a);
        }
        spdlog::info("Font atlas '{}' output alpha range: {}..{}", atlasPath, outMinA, outMaxA);

        TextureOptions options;
        options.width = static_cast<uint32_t>(w);
        options.height = static_cast<uint32_t>(h);
        options.format = PixelFormat::PIXELFORMAT_RGBA8;
        options.mipmaps = false;
        options.minFilter = FilterMode::FILTER_NEAREST;
        options.magFilter = FilterMode::FILTER_NEAREST;
        options.numLevels = 1;
        options.name = "font-atlas";

        auto* texture = new Texture(graphicsDevice.get(), options);
        texture->setEncoding(TextureEncoding::Default);
        const size_t dataSize = pixelCount * 4u;
        texture->setLevelData(0, reinterpret_cast<const uint8_t*>(pixels), dataSize);
        texture->upload();
        stbi_image_free(pixels);

        font->texture = texture;
        if (font->atlasWidth <= 0) font->atlasWidth = w;
        if (font->atlasHeight <= 0) font->atlasHeight = h;
        if (font->lineHeight <= 0.0f) font->lineHeight = 64.0f;
        spdlog::info("Loaded bitmap font '{}': atlas={}x{}, glyphs={}, kerning={}",
            jsonPath, font->atlasWidth, font->atlasHeight, font->glyphs.size(), font->kerning.size());
        return font;
    }
}
