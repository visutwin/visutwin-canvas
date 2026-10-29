// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/dispersion.
//
// Khronos' DispersionTest: rows of glass prisms over a cloth backdrop, each refracting the
// scene colour grab with a different KHR_materials_dispersion strength, so the higher rows split
// white light into fringes of colour. Helipad sky (mip 1), ACES, a dark clear colour, a close
// camera (near 0.01, far 2) framing the model from above and in front, within 0.15 units.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class DispersionExample final : public GltfShowcaseExample
{
public:
    DispersionExample()
        : GltfShowcaseExample({.title = "Dispersion",
          .model = "models/dispersion-test.glb",
          .skyboxMip = 1,
          .cameraPosition = Vector3(0.0f, 0.3f, 1.0f),
          .distanceMax = 0.15f,
          .nearClip = 0.01f,
          .farClip = 2.0f,
          .clearColor = Color(0.2f, 0.2f, 0.2f, 1.0f),
          .sceneColorMap = true})
    {
    }
};

VISUTWIN_EXAMPLE_MAIN(DispersionExample)
