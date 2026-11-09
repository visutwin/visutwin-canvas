// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.11.2026
//
// WebP images, which stb cannot read, decoded through libwebp: a glTF's EXT_texture_webp images
// and texture assets alike (sync and async). An image is told by its bytes (a RIFF container whose
// form type is WEBP), never by its extension, and decoded straight to RGBA8 with row 0 at the TOP,
// as every texture here has it (libwebp never flips).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace visutwin::canvas
{
    /// Whether the bytes are a WebP image.
    bool isWebP(const uint8_t* bytes, size_t size);

    /// Decodes WebP bytes to RGBA8; false (outputs untouched) when libwebp refuses them.
    bool decodeWebP(const uint8_t* bytes, size_t size, std::vector<uint8_t>& rgba, int& width, int& height);

    enum class WebPFile { NotWebP, Decoded, Failed };

    /// A file decoded when it is a WebP image: NotWebP (nothing read beyond its header, `rgba` left
    /// alone) for any other file or a missing one, so the caller's own decoder takes over and reports.
    WebPFile decodeWebPFile(const std::string& path, std::vector<uint8_t>& rgba, int& width, int& height);
}
