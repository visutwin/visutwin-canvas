// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
// Matrix palettes are uploaded ONCE a frame per palette, and the ring that holds them
// GROWS. Both rules live in PaletteFrameAllocator (the bookkeeping of the Metal palette
// ring, with no GPU types) and in the version a SkinInstance or SkinBatchInstance gives
// its palette each time it rewrites it.
//
// What breaks without them is silent at small scale: a fixed 256 KB region holds about
// ten 64-bone characters when each forward and shadow draw takes its own copy, and a draw
// past the end keeps the palette bound before it — another character's pose.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "framework/batching/skinBatchInstance.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/paletteFrameAllocator.h"
#include "scene/graphNode.h"
#include "scene/skin.h"
#include "scene/skinInstance.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr size_t kAlignment = 256;
    constexpr size_t kPalette = 64 * 64;   // 64 bones x float4x4
}

int main()
{
    std::printf("sharing one copy a frame\n");
    {
        PaletteFrameAllocator frame(256 * 1024, kAlignment);
        frame.beginFrame();
        const auto first = frame.allocate(kPalette, 7);
        const auto again = frame.allocate(kPalette, 7);
        const auto other = frame.allocate(kPalette, 8);
        check(first.offset && first.isNew && *first.offset == 0, "a palette's first use is placed and copied");
        check(again.offset && !again.isNew && *again.offset == *first.offset,
            "its next draw gets the same offset and no copy");
        check(other.offset && other.isNew && *other.offset == kPalette, "another palette goes after it");
        check(frame.requestedBytes() == 2 * kPalette, "the frame asked for two palettes, not three");

        const auto unnamedA = frame.allocate(kPalette, 0);
        const auto unnamedB = frame.allocate(kPalette, 0);
        check(unnamedA.isNew && unnamedB.isNew && *unnamedA.offset != *unnamedB.offset,
            "version 0 names nothing: each use is its own copy");

        const auto small = frame.allocate(100, 9);
        const auto next = frame.allocate(100, 10);
        check(small.offset && next.offset && *next.offset - *small.offset == kAlignment,
            "offsets keep the buffer-offset alignment");

        frame.beginFrame();
        const auto newFrame = frame.allocate(kPalette, 7);
        check(newFrame.isNew && *newFrame.offset == 0 && frame.requestedBytes() == kPalette,
            "a new frame shares nothing with the last: the same version is copied again");
    }

    std::printf("\noverflow and growth\n");
    {
        // Room for exactly four palettes.
        PaletteFrameAllocator frame(4 * kPalette, kAlignment);
        frame.beginFrame();
        bool fitted = true;
        for (uint64_t version = 1; version <= 4; ++version) {
            fitted = fitted && frame.allocate(kPalette, version).offset.has_value();
        }
        check(fitted && !frame.overflowed(), "four palettes fill the region exactly without overflowing");

        const auto fifth = frame.allocate(kPalette, 5);
        check(!fifth.offset && !fifth.isNew, "the fifth does not fit: no offset, nothing to copy");
        const auto fifthAgain = frame.allocate(kPalette, 5);
        check(!fifthAgain.offset && frame.requestedBytes() == 5 * kPalette,
            "asked for again (its shadow draw), it is still counted once");
        check(frame.allocate(kPalette, 2).offset.has_value(), "a palette that did fit is still shared after the overflow");
        check(frame.overflowed(), "the frame is marked overflowed");
        check(frame.wantedRegionSize() == 8 * kPalette, "growth at least doubles the region");

        for (uint64_t version = 6; version <= 20; ++version) {
            (void)frame.allocate(kPalette, version);
        }
        check(frame.wantedRegionSize() == 20 * kPalette, "and covers everything the frame asked for when that is more");

        frame.setRegionSize(frame.wantedRegionSize());
        bool allFit = true;
        for (uint64_t version = 1; version <= 20; ++version) {
            allFit = allFit && frame.allocate(kPalette, version).offset.has_value();
        }
        check(allFit && !frame.overflowed(), "the same frame fits the grown region");
    }

    std::printf("\npalette versions\n");
    {
        const uint64_t a = GraphicsDevice::nextPaletteVersion();
        const uint64_t b = GraphicsDevice::nextPaletteVersion();
        check(a != 0 && b != 0 && a != b, "versions are never 0 and never repeat");

        std::vector<GraphNode> nodes(2);
        SkinBatchInstance batch({&nodes[0], &nodes[1]});
        const uint64_t created = batch.paletteVersion();
        check(created != 0, "a batch palette is named from the start");
        batch.updateMatrices();
        const uint64_t updated = batch.paletteVersion();
        check(updated != created, "rewriting the palette renames it");
        SkinBatchInstance otherBatch({&nodes[0]});
        check(otherBatch.paletteVersion() != updated && otherBatch.paletteVersion() != created,
            "two palettes never share a name");

        auto skin = std::make_shared<Skin>(std::vector<Matrix4>(2, Matrix4::identity()),
            std::vector<std::string>{"a", "b"});
        SkinInstance skinA(skin);
        SkinInstance skinB(skin);
        skinA.setBones({&nodes[0], &nodes[1]});
        check(skinA.paletteVersion() != 0 && skinA.paletteVersion() != skinB.paletteVersion(),
            "two skin instances have different names");

        SkinInstance::beginFrame();
        const uint64_t before = skinA.paletteVersion();
        skinA.updateMatrixPalette(&nodes[0]);
        const uint64_t thisFrame = skinA.paletteVersion();
        check(thisFrame != before, "a skin's palette is renamed when the frame's update rewrites it");
        skinA.updateMatrixPalette(&nodes[0]);
        check(skinA.paletteVersion() == thisFrame,
            "and keeps the name for the rest of the frame: the shadow and forward draws share one copy");
        SkinInstance::beginFrame();
        skinA.updateMatrixPalette(&nodes[0]);
        check(skinA.paletteVersion() != thisFrame, "the next frame's update renames it again");
    }

    return finish("palette sharing");
}
