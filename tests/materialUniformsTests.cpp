// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
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

using namespace visutwin::canvas;

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

    bool near(const float a, const float b)
    {
        return std::fabs(a - b) < 1e-5f;
    }
}

int main()
{
    // A mutation after the first pack must be visible in the next pack.
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(1.0f, 0.0f, 0.0f, 1.0f));
        const MaterialUniforms& first = material->packedUniforms();
        passed &= expect(near(first.baseColor[0], 1.0f) && near(first.baseColor[1], 0.0f),
            "first pack reflects the diffuse colour");

        material->setDiffuse(Color(0.0f, 1.0f, 0.0f, 1.0f));
        const MaterialUniforms& second = material->packedUniforms();
        passed &= expect(near(second.baseColor[0], 0.0f) && near(second.baseColor[1], 1.0f),
            "a setter after the first pack invalidates the cache");
    }

    // Scalars go through the same path.
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setMetalness(0.0f);
        material->packedUniforms();
        material->setMetalness(1.0f);
        passed &= expect(near(material->packedUniforms().metallicFactor, 1.0f),
            "metalness edit survives the cache");
    }

    // Repeated reads with no edit in between must agree — this is the cached path.
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setOpacity(0.25f);
        const float a = material->packedUniforms().baseColor[3];
        const float b = material->packedUniforms().baseColor[3];
        passed &= expect(near(a, b) && near(a, 0.25f),
            "repeated packs agree when nothing changed");
    }

    // Parameter overrides bypass the typed setters, so setParameter must dirty too.
    {
        auto material = std::make_shared<StandardMaterial>();
        material->setDiffuse(Color(1.0f, 1.0f, 1.0f, 1.0f));
        material->packedUniforms();
        material->setParameter("material_baseColor", Color(0.0f, 0.0f, 1.0f, 1.0f));
        const MaterialUniforms& after = material->packedUniforms();
        passed &= expect(near(after.baseColor[2], 1.0f) && near(after.baseColor[0], 0.0f),
            "setParameter override invalidates the cache");
    }

    return passed ? 0 : 1;
}
