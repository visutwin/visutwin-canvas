// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream test/specular.
//
// Khronos' SpecularTest grid, checking KHR_materials_specular: rows 1 and 2, 3 and 4, and 5 and 6
// should each look identical (the factor alone against a texture alone with the matching factor),
// and the last sphere of row 7 should look like a mirror ball. The environment lighting is what
// shows the dielectric F0 the extension changes, so the scene is lit by the helipad atlas (also
// the sky) at exposure 10 and a white directional light at (45, 180, 0), no shadows. The camera
// clears to dark grey with ACES; the orbit camera frames the model, then is set level with it
// (pitch 0) at yaw 0.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class SpecularExample final : public GltfShowcaseExample
{
public:
    SpecularExample()
        : GltfShowcaseExample({.title = "Specular",
          .model = "models/SpecularTest.glb",
          .cameraPitch = 0.0f,
          .cameraYaw = 0.0f,
          .clearColor = Color(0.1f, 0.1f, 0.1f, 1.0f),
          .lightColor = Color(1.0f, 1.0f, 1.0f, 1.0f)})
    {
    }

protected:
    bool create() override
    {
        if (!GltfShowcaseExample::create()) {
            return false;
        }
        scene()->setExposure(10.0f);
        return true;
    }
};

VISUTWIN_EXAMPLE_MAIN(SpecularExample)
