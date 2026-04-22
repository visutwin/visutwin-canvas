// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    // Atlas layout (for size=512):
    //   Mipmaps (direct resample):
    //     level 0: rect(0, 0, 512, 256)
    //     level 1..6: diagonal shrink
    //   Reflections (GGX prefiltered), left column of lower half:
    //     blur 1: rect(0, 256, 256, 128)  specularPower=512
    //     blur 2..6: shrinking vertically
    //   Ambient (Lambert irradiance):
    //     rect(128, 384, 64, 32)
    class EnvLighting
    {
    public:
        static Texture* generateAtlas(GraphicsDevice* device, Texture* source,
                                      int size = 512,
                                      int numReflectionSamples = 1024,
                                      int numAmbientSamples = 2048);

        static Texture* generateSkyboxCubemap(GraphicsDevice* device, Texture* source, int size = 0);

    private:
        static void dirToEquirectUv(float x, float y, float z, float& u, float& v);

        static void hemisphereSampleGGX(float& hx, float& hy, float& hz, float xi1, float xi2, float a);
        static void hemisphereSampleLambert(float& hx, float& hy, float& hz, float xi1, float xi2);
        static float D_GGX(float NoH, float linearRoughness);

        static std::array<uint8_t, 4> encodeRGBP(float r, float g, float b);

        static int getRequiredSamplesGGX(int numSamples, int specularPower);

        static std::vector<float> generateLambertSampleTable(int numSamples, int sourceTotalPixels);
        static std::vector<float> generateGGXSampleTable(int numSamples, int specularPower, int sourceTotalPixels);
    };
}
