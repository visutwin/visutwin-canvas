// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/anisotropy-strength.
//
// Khronos' AnisotropyStrengthTest: a grid of spheres whose KHR_materials_anisotropy strength
// rises along one axis and roughness along the other, stretching the highlight further on each.
// Morning sky (turned 70 degrees, intensity 0.5), a yellow directional light, ACES, the camera at
// yaw 90 and distance 12.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class AnisotropyStrengthExample final : public GltfShowcaseExample
{
public:
    AnisotropyStrengthExample()
        : GltfShowcaseExample({.title = "Anisotropy Strength",
          .model = "models/AnisotropyStrengthTest.glb",
          .envAtlas = "cubemaps/morning-env-atlas.png",
          .skyboxIntensity = 0.5f,
          .skyboxYaw = 70.0f,
          .modelEuler = Vector3(40.0f, 90.0f, 0.0f),
          .modelPosition = Vector3(0.0f, 0.0f, 1.0f),
          .modelScale = Vector3(0.8f, 0.8f, 0.8f),
          .cameraYaw = 90.0f,
          .cameraDistance = 12.0f,
          .lightColor = Color(1.0f, 1.0f, 0.0f, 1.0f)})
    {
    }
};

VISUTWIN_EXAMPLE_MAIN(AnisotropyStrengthExample)
