// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// CPU-based environment lighting atlas generation.
//
// DEVIATION: upstream performs reprojection on GPU via fragment shaders.
// This port performs all convolution work on CPU (importance sampling + RGBP encoding)
// and uploads the final atlas as an RGBA8 texture. This is acceptable because atlas
// generation is a one-time offline operation at load time.
//
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace visutwin::canvas
{
    class GraphicsDevice;
    class Texture;

    /**
     * Helper class to support prefiltering lighting data.
     *
     * Generates a 512x512 RGBP environment atlas from an HDR equirectangular source texture.
     * The atlas layout matches what the Metal shader's mapRoughnessUv() / decodeRGBP() consume:
     *
     *   Mipmaps section (cubemap -> equirect, direct resample):
     *     level 0: rect(0, 0, 512, 256)
     *     level 1: rect(256, 256, 256, 128)
     *     level 2: rect(384, 384, 128, 64)
     *     level 3: rect(448, 448, 64, 32)
     *     level 4: rect(480, 480, 32, 16)
     *     level 5: rect(496, 496, 16, 8)
     *     level 6: rect(504, 504, 8, 4)
     *
     *   Reflections section (GGX prefiltered):
     *     blur 1: rect(0, 256, 256, 128)  specularPower=512
     *     blur 2: rect(0, 384, 128, 64)   specularPower=128
     *     blur 3: rect(0, 448, 64, 32)    specularPower=32
     *     blur 4: rect(0, 480, 32, 16)    specularPower=8
     *     blur 5: rect(0, 496, 16, 8)     specularPower=2
     *     blur 6: rect(0, 504, 8, 4)      specularPower=1
     *
     *   Ambient section (Lambert irradiance):
     *     rect(128, 384, 64, 32)
     */
    class EnvLighting
    {
    public:
        /**
         * Generate a 512x512 RGBP atlas from an HDR equirectangular texture.
         *
         * @param device GraphicsDevice for texture creation.
         * @param source RGBA32F equirectangular texture loaded from .hdr file.
         * @param size Output atlas size (default 512).
         * @param numReflectionSamples Samples for GGX convolution (default 1024).
         * @param numAmbientSamples Samples for Lambert convolution (default 2048).
         * @returns Newly created RGBA8 RGBP-encoded atlas texture.
         */
        static Texture* generateAtlas(GraphicsDevice* device, Texture* source,
                                      int size = 512,
                                      int numReflectionSamples = 1024,
                                      int numAmbientSamples = 2048,
                                      bool useGpu = false);

        /**
         * CPU-only atlas generation from raw RGBA32F equirectangular pixel data.
         * Returns a vector of RGBA8 RGBP-encoded pixels (size*size*4 bytes).
         * No GraphicsDevice or Texture objects required.
         *
         * @param srcData RGBA32F interleaved pixel data (4 floats per pixel).
         * @param srcW Source width in pixels.
         * @param srcH Source height in pixels.
         * @param size Output atlas size (default 512).
         * @param numReflectionSamples Samples for GGX convolution (default 1024).
         * @param numAmbientSamples Samples for Lambert convolution (default 2048).
         */
        static std::vector<uint8_t> generateAtlasRaw(const float* srcData, int srcW, int srcH,
                                                      int size = 512,
                                                      int numReflectionSamples = 1024,
                                                      int numAmbientSamples = 2048);

        /**
         * Generate a high-resolution cubemap from an HDR equirectangular source.
         * EnvLighting.generateSkyboxCubemap()
         *
         * The cubemap is used for rendering the skybox at full source resolution,
         * while the envAtlas (512x512) is used only for PBR lighting (reflections/ambient).
         *
         * @param device GraphicsDevice for texture creation.
         * @param source RGBA32F equirectangular texture loaded from .hdr file.
         * @param size Cubemap face size. 0 = auto (source.width / 4).
         * @returns Newly created RGBA8 cubemap texture (6 faces, no mipmaps).
         */
        static Texture* generateSkyboxCubemap(GraphicsDevice* device, Texture* source, int size = 0);

    private:
        // Internal HDR cubemap representation (float RGBA, 6 faces, with mipmaps)
        struct HdrCubemap
        {
            int size = 0;
            int numLevels = 0;
            // data[mipLevel][face] = vector of float4 pixels (r,g,b,a interleaved)
            std::vector<std::vector<std::vector<float>>> data;
        };

        // Convert equirectangular RGBA32F texture to HDR cubemap
        static HdrCubemap equirectToCubemap(Texture* source, int size);
        static HdrCubemap equirectToCubemap(const float* srcData, int srcW, int srcH, int size);

        // Generate box-filter mipmaps for all cubemap faces
        static void generateMipmaps(HdrCubemap& cubemap);

        // Trilinear sample from HDR cubemap at a given direction and mip level
        struct Color3 { float r, g, b; };
        static Color3 sampleCubemap(const HdrCubemap& cubemap, float dirX, float dirY, float dirZ, float mipLevel);

        // Bilinear sample from a single cubemap face at a given mip level
        static Color3 sampleFace(const std::vector<float>& faceData, int faceSize, float u, float v);

        // Direction -> cubemap face + UV
        static void dirToFaceUv(float x, float y, float z, int& face, float& u, float& v);

        // Direction <-> equirectangular UV conversion
        static void dirToEquirectUv(float x, float y, float z, float& u, float& v);
        static void equirectUvToDir(float u, float v, float& x, float& y, float& z);

        // Hemisphere sampling for importance sampling
        static void hemisphereSampleGGX(float& hx, float& hy, float& hz, float xi1, float xi2, float a);
        static void hemisphereSampleLambert(float& hx, float& hy, float& hz, float xi1, float xi2);

        // GGX normal distribution function
        static float D_GGX(float NoH, float linearRoughness);

        // RGBP encoding: linear HDR float3 -> RGBA8
        static std::array<uint8_t, 4> encodeRGBP(float r, float g, float b);

        // Write a convolved equirectangular region into the atlas buffer (from cubemap)
        static void writeEquirectRegion(uint8_t* atlas, int atlasSize,
                                        int rx, int ry, int rw, int rh,
                                        const HdrCubemap& cubemap, float mipLevel);

        // Write an equirectangular region sampling directly from source equirect texture.
        // reprojectTexture with numSamples=1 samples the source directly
        // without going through a cubemap, preserving full source resolution.
        static void writeEquirectFromSource(uint8_t* atlas, int atlasSize,
                                            int rx, int ry, int rw, int rh,
                                            const float* srcData, int srcW, int srcH);

        // Write a GGX-prefiltered equirectangular region into the atlas
        static void writeGGXRegion(uint8_t* atlas, int atlasSize,
                                   int rx, int ry, int rw, int rh,
                                   const HdrCubemap& cubemap,
                                   int specularPower, int numSamples);

        // Write a Lambert-convolved equirectangular region into the atlas
        static void writeLambertRegion(uint8_t* atlas, int atlasSize,
                                       int rx, int ry, int rw, int rh,
                                       const HdrCubemap& cubemap, int numSamples);

        // Required samples table for GGX (accounts for invalid below-hemisphere samples)
        static int getRequiredSamplesGGX(int numSamples, int specularPower);
    };
}
