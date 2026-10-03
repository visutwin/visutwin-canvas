// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2026
//
// The forward sort key decides what order opaque draws go out in, and every defect it
// can have is invisible: the frame still renders, just with more state changes than it
// needed, or with two materials' draws interleaved.
//
// A key that XORs overlapping bit ranges — say the depth-state key and the
// emissive-texture bit both at bit 4 — lets materials differing in one of those hash
// equal; a caller that shifts the result left by 32 discards whichever half it drops.
//
// What is checked here is that each field owns its own bits and that they rank in the
// documented order. A collision sweep stands in for "no field overlaps another",
// because that is the property, not any particular numeric value.
//
// The mesh field is Mesh::id(), a creation-order counter. Ordering one material's draws
// by the mesh's ADDRESS instead makes the order — and with it which of two coplanar
// surfaces is drawn last and wins the depth test — follow the heap, so the same scene
// renders different pixels from run to run. The last block holds the id.
//
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"
#include "scene/renderer/sortKey.h"
#include "support/check.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    // Two mesh ids, adjacent: every id is its own key, none are folded together.
    constexpr uint32_t meshA = 1;
    constexpr uint32_t meshB = 2;
}

int main()
{
    // ── The documented priority order ────────────────────────────────────────
    {
        // A higher bucket sorts after everything in a lower one, whatever else
        // differs. This is the field that exists so a caller can force a block of
        // draws to precede the rest without giving it a layer of its own.
        check(makeForwardSortKey(0, true, 0x7FFFFF, meshB)
            < makeForwardSortKey(1, false, 0, meshA),
            "the draw bucket outranks every other field");

        // Within a bucket, a masked material goes after a plain opaque one — it
        // disables the early depth test, so it must not run first and cost that for
        // everything behind it.
        check(makeForwardSortKey(0, false, 0x7FFFFF, meshB)
            < makeForwardSortKey(0, true, 0, meshA),
            "alpha test outranks the material, so masked draws follow opaque ones");

        // Then material identity, which is what makes consecutive draws skip binding.
        check(makeForwardSortKey(0, false, 7, meshB)
            < makeForwardSortKey(0, false, 8, meshA),
            "material identity outranks the mesh");

        // And the mesh only breaks ties within one material.
        check(makeForwardSortKey(0, false, 7, meshA)
            < makeForwardSortKey(0, false, 7, meshB),
            "the mesh orders draws that share a material");
    }

    // ── Grouping, which is the whole point ───────────────────────────────────
    {
        // Two draws of one material produce keys that no third material's key can
        // fall between — that is what "consecutive draws skip binding" requires, and
        // it is exactly what an overlapping field breaks.
        const uint64_t sameA = makeForwardSortKey(0, false, 5, meshA);
        const uint64_t sameB = makeForwardSortKey(0, false, 5, meshB);
        const uint64_t other = makeForwardSortKey(0, false, 6, meshA);
        check(other > sameA && other > sameB,
            "no other material's key lands between two draws of one material");
    }

    // ── No field overlaps another ────────────────────────────────────────────
    {
        std::set<uint64_t> keys;
        size_t generated = 0;
        for (uint8_t bucket = 0; bucket < 4; ++bucket) {
            for (int alpha = 0; alpha < 2; ++alpha) {
                for (uint32_t material = 0; material < 8; ++material) {
                    for (uint32_t mesh = 0; mesh < 8; ++mesh) {
                        keys.insert(makeForwardSortKey(bucket, alpha != 0, material, mesh));
                        ++generated;
                    }
                }
            }
        }
        check(keys.size() == generated,
            "every distinct combination of the four fields yields a distinct key");
    }

    // ── The mask is a graceful limit, not a wrap into someone else's field ───
    {
        // 23 bits of material id. An id past that wraps and shares a key with
        // another material, which costs a state change — but it must NOT bleed into
        // the alpha-test or bucket bits above it.
        const uint64_t wrapped = makeForwardSortKey(0, false, 0x800000, meshA);
        const uint64_t zero = makeForwardSortKey(0, false, 0, meshA);
        check(wrapped == zero, "a material id past 23 bits wraps within its own field");
        check(makeForwardSortKey(0, false, 0xFFFFFFFF, meshA)
            < makeForwardSortKey(0, true, 0, meshA),
            "an out-of-range material id cannot reach the alpha-test bit");
    }

    // ── The mesh field is an id in creation order, not an address ────────────
    {
        check(makeForwardSortKey(0, false, 7, 0xFFFFFFFFu) < makeForwardSortKey(0, false, 8, 0),
            "the largest mesh id stays inside the mesh field");

        // Meshes get ids in the order they are built, wherever the heap puts them: one
        // is freed in between, so a later mesh may well take its address — which a key
        // made from the address would follow, and the id does not.
        const auto first = std::make_unique<Mesh>();
        auto scratch = std::make_unique<Mesh>();
        const uint32_t scratchId = scratch->id();
        scratch.reset();
        const auto second = std::make_unique<Mesh>();
        const auto third = std::make_unique<Mesh>();
        check(first->id() < scratchId && scratchId < second->id() && second->id() < third->id(),
            "mesh ids follow creation order and are never reused");
        check(makeForwardSortKey(0, false, 7, first->id()) < makeForwardSortKey(0, false, 7, second->id()) &&
              makeForwardSortKey(0, false, 7, second->id()) < makeForwardSortKey(0, false, 7, third->id()),
            "so one material's draws sort in the order their meshes were created");
    }

    // ── The distance sorts take the bucket first ──────────────────────────────
    {
        const uint64_t lowBucket = makeForwardSortKey(10, false, 7, meshA);
        const uint64_t highBucket = makeForwardSortKey(200, false, 7, meshA);
        check(forwardSortKeyBucket(highBucket) == 200 && forwardSortKeyBucket(lowBucket) == 10,
            "the bucket reads back out of the key");

        // Back to front: the higher bucket first, even when it is the nearer draw.
        check(distanceSortsBefore(highBucket, 1.0f, lowBucket, 50.0f, true) &&
              !distanceSortsBefore(lowBucket, 50.0f, highBucket, 1.0f, true),
            "back to front draws the higher bucket first, whatever the depths");
        // Front to back: the lower bucket first, even when it is the farther draw.
        check(distanceSortsBefore(lowBucket, 50.0f, highBucket, 1.0f, false) &&
              !distanceSortsBefore(highBucket, 1.0f, lowBucket, 50.0f, false),
            "front to back draws the lower bucket first, whatever the depths");

        // Within one bucket, depth decides, then the key on an exact tie.
        const uint64_t same = makeForwardSortKey(MeshInstance::kDefaultDrawBucket, false, 7, meshA);
        const uint64_t sameLater = makeForwardSortKey(MeshInstance::kDefaultDrawBucket, false, 7, meshB);
        check(distanceSortsBefore(same, 9.0f, sameLater, 2.0f, true) &&
              distanceSortsBefore(sameLater, 2.0f, same, 9.0f, false),
            "within a bucket the depth orders the draws");
        check(distanceSortsBefore(same, 3.0f, sameLater, 3.0f, true) &&
              !distanceSortsBefore(sameLater, 3.0f, same, 3.0f, true),
            "and an exact tie falls back to the forward key");

        // Every mesh instance starts in the middle bucket, so moving one either way
        // reorders it against all the rest.
        const MeshInstance instance(static_cast<Mesh*>(nullptr), static_cast<Material*>(nullptr));
        check(instance.drawBucket() == 127, "a mesh instance's draw bucket defaults to 127");
    }

    return finish("forward sort key");
}
