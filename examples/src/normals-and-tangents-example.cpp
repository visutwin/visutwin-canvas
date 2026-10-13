// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/normals-and-tangents.
//
// Khronos' NormalTangentTest: a grid of spheres whose normal map, read through the mesh's
// own tangents, should light every sphere the same way: a bump lit from the top right, the
// same on the front and back faces. Helipad sky (turned 70 degrees, intensity 1.5), ACES, and
// a white shadow-casting directional light at (45, 180, 0) (bias 0.2, normal offset 0.05, a
// 2048 map). The model is turned 90 degrees, moved to (0, 0, 1) and scaled by 0.8; the orbit
// camera is set level with it (pitch 0) at yaw 90 and distance 3.
//
// DEVIATION: upstream's orbitCamera script is the examples' CameraControls in orbit mode, started at
// the pose the script reaches (gltfShowcase.h); there is no inertia.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class NormalsAndTangentsExample final : public GltfShowcaseExample
{
public:
    NormalsAndTangentsExample()
        : GltfShowcaseExample({.title = "Normals and Tangents",
          .model = "models/NormalTangentTest.glb",
          .skyboxIntensity = 1.5f,
          .skyboxYaw = 70.0f,
          .modelEuler = Vector3(0.0f, 90.0f, 0.0f),
          .modelPosition = Vector3(0.0f, 0.0f, 1.0f),
          .modelScale = Vector3(0.8f, 0.8f, 0.8f),
          .cameraPitch = 0.0f,
          .cameraYaw = 90.0f,
          .cameraDistance = 3.0f})
    {
    }

protected:
    bool create() override
    {
        if (!GltfShowcaseExample::create()) {
            return false;
        }

        // A white directional light casting shadows
        Entity* light = createDirectionalLight(Vector3(45.0f, 180.0f, 0.0f), Color(1.0f, 1.0f, 1.0f, 1.0f), 1.0f, true);
        if (auto* lightComponent = light->findComponent<LightComponent>()) {
            lightComponent->setShadowBias(0.2f);
            lightComponent->setShadowNormalBias(0.05f);
            lightComponent->setShadowResolution(2048);
        }
        return true;
    }
};

VISUTWIN_EXAMPLE_MAIN(NormalsAndTangentsExample)
