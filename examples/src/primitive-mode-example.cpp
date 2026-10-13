// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream test/primitive-mode.
//
// Khronos' PrimitiveModeNormalsTest: the same shape drawn as points, line strips and triangles,
// without normals on the left and with them on the right. glTF requires flat shading where the
// normals are missing, so those triangles are faceted; the points and lines there should come
// out unlit. Morning sky (intensity 1), ACES, and a yellow directional light at (45, 180, 0), no
// shadows. The model is turned 90 degrees; the orbit camera frames it at yaw 90 and distance 25.
//
// DEVIATION: upstream's orbitCamera script is the examples' CameraControls in orbit mode, started at
// the pose the script reaches (gltfShowcase.h); there is no inertia.
// DEVIATION: a glTF point primitive is drawn by the engine's point path, which carries no normal
// and is always unlit, and draws a point 3 pixels across on Metal (1 on Vulkan). The points WITH
// normals therefore come out unlit too, and on Metal fill each sphere solid where upstream's
// one-pixel points leave it speckled.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class PrimitiveModeExample final : public GltfShowcaseExample
{
public:
    PrimitiveModeExample()
        : GltfShowcaseExample({.title = "Primitive Mode",
          .model = "models/PrimitiveModeNormalsTest.glb",
          .envAtlas = "cubemaps/morning-env-atlas.png",
          .skyboxIntensity = 1.0f,
          .modelEuler = Vector3(0.0f, 90.0f, 0.0f),
          .cameraYaw = 90.0f,
          .cameraDistance = 25.0f,
          .lightColor = Color(1.0f, 1.0f, 0.0f, 1.0f)})
    {
    }
};

VISUTWIN_EXAMPLE_MAIN(PrimitiveModeExample)
