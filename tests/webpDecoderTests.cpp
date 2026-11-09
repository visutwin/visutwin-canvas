// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// The shared WebP decoder (framework/parsers/texture/webpDecoder.h), which the glTF parser, texture
// assets and the async texture loader all go through: a WebP image is told by its bytes, decoded to
// RGBA8 with row 0 at the TOP, and any other file is left to the caller's own decoder. The oracle is
// an independent decode of the same files (Pillow): the image mean, and the means of its top and
// bottom eighths, which differ, so a flipped image fails.

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "framework/parsers/texture/webpDecoder.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    double meanOfRows(const std::vector<uint8_t>& rgba, const int width, const int firstRow, const int lastRow)
    {
        double sum = 0.0;
        for (int y = firstRow; y < lastRow; ++y) {
            for (int x = 0; x < width * 4; ++x) {
                sum += rgba[static_cast<size_t>(y) * width * 4 + x];
            }
        }
        return sum / (static_cast<double>(lastRow - firstRow) * width * 4);
    }
}

int main()
{
    const std::string assets = VISUTWIN_ASSETS_DIR;

    std::cout << "telling a WebP image by its bytes\n";
    {
        const uint8_t webp[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
        const uint8_t wave[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
        check(isWebP(webp, sizeof(webp)), "a RIFF container of form WEBP is WebP");
        check(!isWebP(wave, sizeof(wave)), "another RIFF form is not");
        check(!isWebP(webp, 11), "nor are too few bytes");
        check(!isWebP(nullptr, 12), "nor nothing");
    }

    std::cout << "\nother files are the caller's\n";
    {
        std::vector<uint8_t> rgba = {7};
        int width = -1;
        int height = -1;
        check(decodeWebPFile(assets + "/textures/checkboard.png", rgba, width, height) == WebPFile::NotWebP &&
                  rgba.size() == 1 && width == -1, "a PNG is not decoded and nothing is touched");
        check(decodeWebPFile(assets + "/textures/no-such-file.webp", rgba, width, height) == WebPFile::NotWebP,
            "a missing file is left to the caller to report");
        const uint8_t truncated[16] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
        check(!decodeWebP(truncated, sizeof(truncated), rgba, width, height) && width == -1,
            "bytes libwebp refuses fail and leave the outputs alone");
    }

    std::cout << "\ndecoding\n";
    {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        check(decodeWebPFile(assets + "/textures/vfx/explosion-flipbook.webp", rgba, width, height) ==
                  WebPFile::Decoded, "a WebP file decodes");
        check(width == 2048 && height == 2048 && rgba.size() == 2048u * 2048u * 4u, "to RGBA8 at its size");
        if (rgba.size() == 2048u * 2048u * 4u) {
            check(near(meanOfRows(rgba, width, 0, height), 91.931, 0.5), "with the image's mean");
            const double top = meanOfRows(rgba, width, 0, height / 8);
            const double bottom = meanOfRows(rgba, width, height * 7 / 8, height);
            check(near(top, 86.388, 0.5) && near(bottom, 74.576, 0.5),
                "row 0 at the top (top eighth " + std::to_string(top) + ", bottom " + std::to_string(bottom) + ")");
        }
    }

    return finish("webp decoder");
}
