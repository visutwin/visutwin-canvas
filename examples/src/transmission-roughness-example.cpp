// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/transmission-roughness.
//
// Khronos' TransmissionRoughnessTest: transmissive spheres and slabs whose roughness rises
// along each row, so the scene seen through them blurs more and more. Table-mountain sky
// (turned 70 degrees, intensity 1.5), a white directional light, ACES, the scene colour grab,
// the camera at yaw 90 and distance 2.
//
// DEVIATION: upstream's orbitCamera script is the examples' CameraControls in orbit mode, started at
// the pose the script reaches (gltfShowcase.h); there is no inertia.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class TransmissionRoughnessExample final : public GltfShowcaseExample
{
public:
    TransmissionRoughnessExample()
        : GltfShowcaseExample({.title = "Transmission Roughness",
          .model = "models/TransmissionRoughnessTest.glb",
          .envAtlas = "cubemaps/table-mountain-env-atlas.png",
          .skyboxIntensity = 1.5f,
          .skyboxYaw = 70.0f,
          .modelEuler = Vector3(0.0f, 90.0f, 0.0f),
          .cameraYaw = 90.0f,
          .cameraDistance = 2.0f,
          .clearColor = Color(0.1f, 0.1f, 0.1f, 1.0f),
          .sceneColorMap = true,
          .lightColor = Color(1.0f, 1.0f, 1.0f, 1.0f)})
    {
    }
};

VISUTWIN_EXAMPLE_MAIN(TransmissionRoughnessExample)
