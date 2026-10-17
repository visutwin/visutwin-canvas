// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 17.10.2026
//
// The cloud noise of DynamicSkyEffect is byte for byte upstream's: the three textures hashed
// against the bytes upstream's own generator writes (its functions run under Node, FNV-1a 64 of
// the data as stored, layers in order). A hash, an equalization or a float32 rounding that
// differs moves every cloud; a frame would show only that the clouds look different.

#include <cstdint>
#include <iostream>
#include <vector>

#include "scene/graphics/dynamicSkyNoise.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    uint64_t fnv1a(const std::vector<uint8_t>& data)
    {
        uint64_t hash = 0xcbf29ce484222325ull;
        for (const uint8_t byte : data) {
            hash ^= byte;
            hash *= 0x100000001b3ull;
        }
        return hash;
    }
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "the hash\n";
    check(dynamic_sky_noise::hash2(0, 0, 0) >= 0.0 && dynamic_sky_noise::hash2(0, 0, 0) <= 1.0, "in 0-1");

    std::cout << "the textures, as upstream writes them\n";
    const auto cloud = dynamic_sky_noise::cloudMapData(256);
    check(cloud.size() == 256u * 256u * 4u && fnv1a(cloud) == 0xfde7a757339fc627ull, "the cloud map (256^2 RGBA8)");
    const auto shape = dynamic_sky_noise::shapeMapData(64);
    check(shape.size() == 64u * 64u * 64u && fnv1a(shape) == 0xac49e76837a49f29ull, "the shapes (64^3 R8)");
    const auto detail = dynamic_sky_noise::detailMapData(32);
    check(detail.size() == 32u * 32u * 32u && fnv1a(detail) == 0x81370bf6563e15a9ull, "the detail (32^3 R8)");

    return finish("dynamic sky noise");
}
