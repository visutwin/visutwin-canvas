// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// The icon glyphs the overlay gizmos mark entities with, named for the component they mark,
// and their textures: white, the glyph's coverage in alpha, shared per device.
//
// DEVIATION: upstream draws each glyph as SVG and has the browser rasterise it. Here each is
// the same drawing spelled as strokes 1.6 units wide (lines, arcs, circles), filled circles and
// filled polygons in a 24 x 24 box centred on the origin, y down, rasterised on the CPU with
// 4 x 4 samples a pixel at 4 pixels a unit; a joint of a stroked outline is mitred where SVG
// mitres it (each joint here is a right angle) and rounded where it rounds it.
//
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    /// An entity with several of the components shows the first glyph in this order.
    enum class OverlayIcon
    {
        Camera,
        Light,
        AudioListener,
        Sound,
        ParticleSystem,
        Anim,
        Animation,
        Script,
        Count
    };

    constexpr size_t kOverlayIconCount = static_cast<size_t>(OverlayIcon::Count);

    /// The icon's name ('camera', 'light', 'audiolistener', ...).
    const char* overlayIconName(OverlayIcon icon);

    /// The glyph as `size` x `size` RGBA8 pixels, top row first: white, its coverage in alpha.
    std::vector<uint8_t> rasterizeOverlayIcon(OverlayIcon icon, int size);

    /// The textures of every glyph on a device (96 pixels square, mipmapped, clamped), created
    /// once and shared while anything holds them.
    struct OverlayIconTextures
    {
        std::array<std::shared_ptr<Texture>, kOverlayIconCount> textures;
    };
    std::shared_ptr<OverlayIconTextures> getOverlayIconTextures(const std::shared_ptr<GraphicsDevice>& device);
}
