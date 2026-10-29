// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
// Port of upstream materials/anisotropy-lamp.
//
// Khronos' AnisotropyBarnLamp: a brushed-metal barn lamp whose shade carries an anisotropy
// direction map, with a clearcoat and a glass bulb (transmission, so the scene colour grab is
// taken: the depth layer is moved to the third opaque slot, after the lamp's opaque parts).
// Morning sky (turned 70 degrees, intensity 0.5, mip 1), ACES, a close camera at yaw 90 and
// distance 0.3, no light of its own.
//
#include "../gltfShowcase.h"

using namespace visutwin::canvas;

class AnisotropyLampExample final : public GltfShowcaseExample
{
public:
    AnisotropyLampExample()
        : GltfShowcaseExample({.title = "Anisotropy Lamp",
          .model = "models/AnisotropyBarnLamp.glb",
          .envAtlas = "cubemaps/morning-env-atlas.png",
          .skyboxIntensity = 0.5f,
          .skyboxMip = 1,
          .skyboxYaw = 70.0f,
          .modelPosition = Vector3(0.0f, 0.0f, 1.0f),
          .modelScale = Vector3(0.8f, 0.8f, 0.8f),
          .cameraYaw = 90.0f,
          .cameraDistance = 0.3f,
          .sceneColorMap = true,
          .depthLayerOpaqueIndex = 2})
    {
    }
};

VISUTWIN_EXAMPLE_MAIN(AnisotropyLampExample)
