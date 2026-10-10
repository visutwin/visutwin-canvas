// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 22.08.2026
//
// Material packs its GPU uniform block once and reuses it until something on the
// material changes (Material::packedUniforms). These cover the invalidation: a
// mutator that fails to mark the cache dirty returns yesterday's values, which
// shows up as a surface that ignores an edit rather than as an obvious failure.

#include <cmath>
#include <iostream>
#include <memory>
#include <string_view>

#include "scene/materials/standardMaterial.h"
#include "support/check.h"

using namespace visutwin::canvas;
using visutwin::canvas::test::nearStrict;

namespace
{
    bool passed = true;

    bool expect(const bool condition, const std::string_view message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << "\n";
        }
        return condition;
    }

    constexpr float kTolerance = 1e-5f;
}

int main()
{
    // A mutation after the first pack must be visible in the next pack.
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(1.0f, 0.0f, 0.0f, 1.0f));
        const MaterialUniforms& first = material->packedUniforms();
        passed &= expect(nearStrict(first.baseColor[0], 1.0f, kTolerance) &&
                         nearStrict(first.baseColor[1], 0.0f, kTolerance),
            "first pack reflects the diffuse colour");

        material->setDiffuse(Color(0.0f, 1.0f, 0.0f, 1.0f));
        const MaterialUniforms& second = material->packedUniforms();
        passed &= expect(nearStrict(second.baseColor[0], 0.0f, kTolerance) &&
                         nearStrict(second.baseColor[1], 1.0f, kTolerance),
            "a setter after the first pack invalidates the cache");
    }

    // Scalars go through the same path.
    {
        auto material = std::make_shared<StandardMaterial>();
        // Metalness only packs in the metalness workflow; the default is the
        // specular workflow, which packs metallic 0 whatever `metalness` says.
        material->setUseMetalness(true);
        material->setMetalness(0.0f);
        material->packedUniforms();
        material->setMetalness(1.0f);
        passed &= expect(nearStrict(material->packedUniforms().metallicFactor, 1.0f, kTolerance),
            "metalness edit survives the cache");
    }

    // Repeated reads with no edit in between must agree — this is the cached path.
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setOpacity(0.25f);
        const float a = material->packedUniforms().baseColor[3];
        const float b = material->packedUniforms().baseColor[3];
        passed &= expect(nearStrict(a, b, kTolerance) && nearStrict(a, 0.25f, kTolerance),
            "repeated packs agree when nothing changed");
    }

    // Parameter overrides bypass the typed setters, so setParameter must dirty too.
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        material->packedUniforms();
        material->setParameter("material_baseColor", Color(0.0f, 0.0f, 1.0f, 1.0f));
        const MaterialUniforms& after = material->packedUniforms();
        passed &= expect(nearStrict(after.baseColor[2], 1.0f, kTolerance) &&
                         nearStrict(after.baseColor[0], 0.0f, kTolerance),
            "setParameter override invalidates the cache");
    }

    // The opacity map reads its own channel under its own transform: alpha and identity by
    // default, so a material that never sets them reads what it always did; a diffuse
    // transform must not move the opacity map.
    {
        auto material = std::make_shared<StandardMaterial>();
        const MaterialUniforms& defaults = material->packedUniforms();
        passed &= expect(nearStrict(defaults.opacityTransform0[3], 3.0f, kTolerance),
            "the opacity map reads alpha by default");
        passed &= expect(nearStrict(defaults.opacityTransform0[0], 1.0f, kTolerance) &&
                         nearStrict(defaults.opacityTransform1[1], 1.0f, kTolerance) &&
                         nearStrict(defaults.opacityTransform0[2], 0.0f, kTolerance) &&
                         nearStrict(defaults.opacityTransform1[2], 0.0f, kTolerance),
            "the opacity map's transform is the identity by default");

        material->setDiffuseMapOffset(Vector2(0.25f, 0.0f));
        material->setOpacityMapChannel(MapChannel::MAP_CHANNEL_R);
        material->setOpacityMapTiling(Vector2(2.0f, 3.0f));
        material->setOpacityMapOffset(Vector2(0.5f, 0.125f));
        const MaterialUniforms& set = material->packedUniforms();
        passed &= expect(nearStrict(set.opacityTransform0[3], 0.0f, kTolerance),
            "setOpacityMapChannel selects the channel");
        passed &= expect(nearStrict(set.opacityTransform0[0], 2.0f, kTolerance) &&
                         nearStrict(set.opacityTransform1[1], 3.0f, kTolerance) &&
                         nearStrict(set.opacityTransform0[2], 0.5f, kTolerance) &&
                         nearStrict(set.opacityTransform1[2], 1.0f - 3.0f - 0.125f, kTolerance),
            "the opacity map packs its own tiling and offset");
        passed &= expect(nearStrict(set.baseColorTransform0[2], 0.25f, kTolerance),
            "the diffuse transform stays the diffuse map's");
    }

    return passed ? 0 : 1;
}
