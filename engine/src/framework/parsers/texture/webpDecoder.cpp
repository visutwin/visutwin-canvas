// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
#include "webpDecoder.h"

#include <cstring>
#include <fstream>

#include <webp/decode.h>

namespace visutwin::canvas
{
    bool isWebP(const uint8_t* bytes, const size_t size)
    {
        return bytes && size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WEBP", 4) == 0;
    }

    bool decodeWebP(const uint8_t* bytes, const size_t size, std::vector<uint8_t>& rgba, int& width, int& height)
    {
        int w = 0;
        int h = 0;
        uint8_t* decoded = WebPDecodeRGBA(bytes, size, &w, &h);
        if (!decoded || w <= 0 || h <= 0) {
            if (decoded) {
                WebPFree(decoded);
            }
            return false;
        }
        rgba.assign(decoded, decoded + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
        WebPFree(decoded);
        width = w;
        height = h;
        return true;
    }

    WebPFile decodeWebPFile(const std::string& path, std::vector<uint8_t>& rgba, int& width, int& height)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return WebPFile::NotWebP;
        }
        const auto size = static_cast<size_t>(file.tellg());
        uint8_t header[12] = {};
        file.seekg(0);
        if (size < sizeof(header) || !file.read(reinterpret_cast<char*>(header), sizeof(header)) ||
            !isWebP(header, sizeof(header))) {
            return WebPFile::NotWebP;
        }
        std::vector<uint8_t> bytes(size);
        file.seekg(0);
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
            return WebPFile::Failed;
        }
        return decodeWebP(bytes.data(), bytes.size(), rgba, width, height) ? WebPFile::Decoded : WebPFile::Failed;
    }
}
