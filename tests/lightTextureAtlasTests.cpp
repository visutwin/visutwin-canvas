// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// The clustered shadow atlas is one packed texture: a slot per light, an omni
// light's six faces in a 3x2 grid of tiles inside its slot, and a shader that picks
// the face and UV from the light-to-fragment direction. A layout error here does
// not read as an error in a render — a face sampled from its neighbour's tile is
// still a plausible shadow, just the wrong one — so the pure parts are held here:
// the split, the tile layout, and the face lookup against the six face cameras'
// REAL projection, which is the only oracle that cannot share a convention bug
// with the shader it mirrors.
//
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "scene/lighting/lightTextureAtlas.h"
#include "scene/renderer/lightCamera.h"

using namespace visutwin::canvas;

namespace
{
    int failures = 0;

    void check(const bool condition, const char* what)
    {
        if (!condition) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }

    bool rectsOverlap(const Vector4& a, const Vector4& b)
    {
        constexpr float eps = 1e-6f;
        return a.getX() < b.getX() + b.getZ() - eps && b.getX() < a.getX() + a.getZ() - eps &&
               a.getY() < b.getY() + b.getW() - eps && b.getY() < a.getY() + a.getW() - eps;
    }

    bool rectInside(const Vector4& inner, const Vector4& outer)
    {
        constexpr float eps = 1e-5f;
        return inner.getX() >= outer.getX() - eps && inner.getY() >= outer.getY() - eps &&
               inner.getX() + inner.getZ() <= outer.getX() + outer.getZ() + eps &&
               inner.getY() + inner.getW() <= outer.getY() + outer.getW() + eps;
    }

    void checkSplit()
    {
        // The automatic split is the smallest square grid that holds every light.
        check(LightTextureAtlas::automaticSplit(1) == std::vector<int>{1}, "1 light -> 1x1");
        check(LightTextureAtlas::automaticSplit(2) == std::vector<int>{2}, "2 lights -> 2x2");
        check(LightTextureAtlas::automaticSplit(4) == std::vector<int>{2}, "4 lights -> 2x2");
        check(LightTextureAtlas::automaticSplit(5) == std::vector<int>{3}, "5 lights -> 3x3");
        check(LightTextureAtlas::automaticSplit(10) == std::vector<int>{4}, "10 lights -> 4x4");

        const auto one = LightTextureAtlas::subdivide({1});
        check(one.size() == 1 && one[0].getZ() == 1.0f && one[0].getW() == 1.0f, "a 1x1 split is the whole atlas");

        const auto three = LightTextureAtlas::subdivide({3});
        check(three.size() == 9, "a 3x3 split has nine slots");
        for (const auto& slot : three) {
            check(std::fabs(slot.getZ() - 1.0f / 3.0f) < 1e-6f && std::fabs(slot.getW() - 1.0f / 3.0f) < 1e-6f,
                "every slot of an equal split is a third wide");
            check(rectInside(slot, Vector4(0.0f, 0.0f, 1.0f, 1.0f)), "a slot lies inside the atlas");
        }
        for (size_t i = 0; i < three.size(); ++i) {
            for (size_t j = i + 1; j < three.size(); ++j) {
                check(!rectsOverlap(three[i], three[j]), "two slots never overlap");
            }
        }

        // A nested split: the first cell of a 2x2 grid split again into 3x3 gives
        // 3 large slots and 9 small ones, largest first.
        std::vector<int> nested{2, 3, 1, 1, 1};
        const auto mixed = LightTextureAtlas::subdivide(nested);
        check(mixed.size() == 12, "2x2 with one cell split 3x3 has twelve slots");
        check(mixed.size() >= 4 && mixed[0].getZ() == 0.5f && mixed[2].getZ() == 0.5f &&
            std::fabs(mixed[3].getZ() - 0.5f / 3.0f) < 1e-6f, "slots are sorted largest first");
        for (size_t i = 0; i < mixed.size(); ++i) {
            for (size_t j = i + 1; j < mixed.size(); ++j) {
                check(!rectsOverlap(mixed[i], mixed[j]), "nested slots never overlap");
            }
        }
    }

    void checkTiles()
    {
        const Vector4 slot(0.25f, 0.5f, 0.25f, 0.25f);
        std::vector<Vector4> faces;
        for (int face = 0; face < 6; ++face) {
            const Vector4 tile = LightTextureAtlas::omniFaceRect(slot, face);
            check(rectInside(tile, slot), "an omni face tile lies inside its slot");
            check(std::fabs(tile.getZ() - slot.getZ() / 3.0f) < 1e-6f, "a face tile is a third of the slot");
            // Column = axis, row = sign: the order the shader's tileOffset uses.
            const float col = static_cast<float>(face / 2);
            const float row = static_cast<float>(face % 2);
            check(std::fabs(tile.getX() - (slot.getX() + col * tile.getZ())) < 1e-6f &&
                  std::fabs(tile.getY() - (slot.getY() + row * tile.getZ())) < 1e-6f,
                "face tiles sit at (axis, sign) in the 3x3 grid");
            faces.push_back(tile);
        }
        for (size_t i = 0; i < faces.size(); ++i) {
            for (size_t j = i + 1; j < faces.size(); ++j) {
                check(!rectsOverlap(faces[i], faces[j]), "two face tiles never overlap");
            }
        }

        const Vector4 spot = LightTextureAtlas::spotViewport(slot, 2048, 4);
        const float inset = 4.0f / 2048.0f;
        check(std::fabs(spot.getX() - (slot.getX() + inset)) < 1e-7f &&
              std::fabs(spot.getZ() - (slot.getZ() - 2.0f * inset)) < 1e-7f,
            "a spot viewport is its slot inset by the edge pixels");
    }

    // The face lookup the shader performs, held against what the face cameras
    // actually render: for a direction d, the face whose camera sees d in front and
    // inside its 90-degree frustum must be the face the lookup names, and the
    // camera's projected NDC, mapped to a top-left-origin UV the way the viewport
    // bias maps it, must be the lookup's UV.
    void checkFaceLookupAgainstCameras()
    {
        const Matrix4 projection = Matrix4::perspective(90.0f, 1.0f, 0.01f, 100.0f);
        std::mt19937 rng(20260916u);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);

        int checked = 0;
        for (int sample = 0; sample < 4000; ++sample) {
            const Vector3 d(unit(rng), unit(rng), unit(rng));
            if (d.lengthSquared() < 1e-3f) {
                continue;
            }
            int face = -1;
            const Vector2 uv = LightTextureAtlas::cubemapFaceCoordinates(d, face);
            check(face >= 0 && face < 6, "the lookup names a face");
            if (face < 0 || face >= 6) {
                continue;
            }

            // The face camera: world -> camera space is the inverse of the camera's
            // rotation; it looks down -Z.
            const Quaternion rotation = LightCamera::pointLightRotations[face];
            const Vector3 view = rotation.invert() * d;
            // Second oracle for the same transform, through the matrix path the
            // scene graph uses: a quaternion product that rotated the other way would
            // agree with itself and still be wrong.
            const Matrix4 worldToView = Matrix4::trs(Vector3(0.0f, 0.0f, 0.0f), rotation, Vector3(1.0f, 1.0f, 1.0f)).inverse();
            const Vector3 viewByMatrix = worldToView.transformPoint(d);
            check((viewByMatrix - view).lengthSquared() < 1e-6f, "quaternion and matrix view transforms agree");
            check(view.getZ() < 0.0f, "the named face's camera sees the direction in front of it");
            check(std::fabs(view.getX()) <= -view.getZ() + 1e-4f &&
                  std::fabs(view.getY()) <= -view.getZ() + 1e-4f,
                "the direction lies inside the named face's 90-degree frustum");

            const Vector4 clip = projection * Vector4(view.getX(), view.getY(), view.getZ(), 1.0f);
            if (std::fabs(clip.getW()) < 1e-6f) {
                continue;
            }
            const float ndcX = clip.getX() / clip.getW();
            const float ndcY = clip.getY() / clip.getW();
            const float expectedU = 0.5f + 0.5f * ndcX;
            const float expectedV = 0.5f - 0.5f * ndcY;   // top-left origin, as the viewport bias flips Y
            const bool matches = std::fabs(uv.x - expectedU) < 2e-4f && std::fabs(uv.y - expectedV) < 2e-4f;
            if (!matches) {
                std::printf("  face %d dir (%.3f %.3f %.3f): lookup uv (%.4f %.4f), camera uv (%.4f %.4f)\n",
                    face, d.getX(), d.getY(), d.getZ(), uv.x, uv.y, expectedU, expectedV);
            }
            check(matches, "the lookup UV is the face camera's projected UV");
            ++checked;
        }
        check(checked > 3000, "enough directions were checked");
    }
}

int main()
{
    checkSplit();
    checkTiles();
    checkFaceLookupAgainstCameras();
    if (failures == 0) {
        std::printf("light-texture-atlas: all checks passed\n");
        return 0;
    }
    std::printf("light-texture-atlas: %d check(s) failed\n", failures);
    return 1;
}
