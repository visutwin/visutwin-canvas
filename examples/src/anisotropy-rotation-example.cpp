// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/anisotropy-rotation.
//
// Khronos' AnisotropyRotationTest: discs whose anisotropy direction is turned by
// KHR_materials_anisotropy's rotation, or by a direction map, so the stretched highlight turns
// from one to the next. Morning sky (turned 70 degrees, intensity 1.5), a yellow directional
// light, ACES, the camera at yaw 90 and distance 12.
//
// DEVIATION: upstream's orbitCamera script is the examples' CameraControls in orbit mode, started at
// the pose the script reaches (gltfShowcase.h); there is no inertia.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class AnisotropyRotationExample final : public GltfShowcaseExample
{
public:
    AnisotropyRotationExample()
        : GltfShowcaseExample({.title = "Anisotropy Rotation",
          .model = "models/AnisotropyRotationTest.glb",
          .envAtlas = "cubemaps/morning-env-atlas.png",
          .skyboxIntensity = 1.5f,
          .skyboxYaw = 70.0f,
          .modelEuler = Vector3(-20.0f, 90.0f, 0.0f),
          .modelPosition = Vector3(0.0f, 0.0f, 1.0f),
          .modelScale = Vector3(0.8f, 0.8f, 0.8f),
          .cameraYaw = 90.0f,
          .cameraDistance = 12.0f,
          .lightColor = Color(1.0f, 1.0f, 0.0f, 1.0f)})
    {
    }
};

VISUTWIN_EXAMPLE_MAIN(AnisotropyRotationExample)
