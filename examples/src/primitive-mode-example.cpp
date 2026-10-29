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
// shadows. The model is turned 90 degrees; the camera starts at (27, 0, 3) and orbits (2, 0, 3).
//
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
          .lightColor = Color(1.0f, 1.0f, 0.0f, 1.0f)})
    {
    }

protected:
    bool create() override
    {
        if (!GltfShowcaseExample::create()) {
            return false;
        }
        // The start pose is given outright rather than framed from the model's bounds.
        Entity* camera = _cameraComponent->entity();
        auto* controls = camera->script()
            ? static_cast<CameraControls*>(camera->script()->get(CameraControls::scriptName()))
            : nullptr;
        if (controls) {
            camera->setPosition(27.0f, 0.0f, 3.0f);
            controls->setFocusPoint(Vector3(2.0f, 0.0f, 3.0f));
            controls->storeResetState();
        }
        return true;
    }
};

VISUTWIN_EXAMPLE_MAIN(PrimitiveModeExample)
