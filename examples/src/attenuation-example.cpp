// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream test/attenuation.
//
// Khronos' AttenuationTest: rows of transmissive slabs (KHR_materials_transmission and
// KHR_materials_volume) whose attenuation colour, attenuation distance and thickness vary, so
// the light passing through them is absorbed by different amounts. The camera takes the scene
// colour grab the transmission reads, with linear tone mapping. There is no light and no
// environment (upstream loads the morning atlas but never assigns it), only a 0.9 grey ambient,
// against the camera's default grey clear colour. The model is turned 90 degrees, moved to
// (0, 0, 1) and scaled by 0.8; the orbit camera frames it at yaw 90 and distance 24.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class AttenuationExample final : public GltfShowcaseExample
{
public:
    AttenuationExample()
        : GltfShowcaseExample({.title = "Attenuation",
          .model = "models/AttenuationTest.glb",
          .envAtlas = "cubemaps/morning-env-atlas.png",
          .modelEuler = Vector3(0.0f, 90.0f, 0.0f),
          .modelPosition = Vector3(0.0f, 0.0f, 1.0f),
          .modelScale = Vector3(0.8f, 0.8f, 0.8f),
          .cameraYaw = 90.0f,
          .cameraDistance = 24.0f,
          .toneMapping = TONEMAP_LINEAR,
          .sceneColorMap = true})
    {
    }

protected:
    bool create() override
    {
        if (!GltfShowcaseExample::create()) {
            return false;
        }
        // The atlas is loaded but not used: no environment lighting and no sky.
        scene()->setEnvAtlas(nullptr);
        scene()->setAmbientLight(0.9f, 0.9f, 0.9f);
        return true;
    }
};

VISUTWIN_EXAMPLE_MAIN(AttenuationExample)
