// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/instanceCuller.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/stencilParameters.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "platform/graphics/vulkan/vulkanGraphicsDevice.h"
#include "platform/graphics/vulkan/vulkanShader.h"
#include "platform/graphics/vulkan/vulkanTexture.h"
#include "platform/graphics/vulkan/vulkanUniformRingBuffer.h"
#include "platform/graphics/vulkan/vulkanUtils.h"
#include "scene/lighting/worldClusters.h"
#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "scene/shader-lib/programLibrary.h"
#include "spdlog/spdlog.h"

using namespace visutwin::canvas;

namespace visutwin::canvas
{
    struct VulkanGraphicsDeviceTestAccess
    {
        static void failNextSubmit(
            VulkanGraphicsDevice& device, const VkResult result)
        {
            device._submitResultOverride = result;
        }

        static bool renderingDisabled(const VulkanGraphicsDevice& device)
        {
            return device._renderingDisabled;
        }

        static bool frameActive(const VulkanGraphicsDevice& device)
        {
            return device._frameActive;
        }

        static size_t retiredSwapchainCount(
            const VulkanGraphicsDevice& device)
        {
            return device._retiredSwapchains.size();
        }

        static bool presentFamilySupportsSurface(
            const VulkanGraphicsDevice& device)
        {
            VkBool32 supported = VK_FALSE;
            return vkGetPhysicalDeviceSurfaceSupportKHR(
                       device._physicalDevice, device._presentQueueFamily,
                       device._surface, &supported) == VK_SUCCESS &&
                supported == VK_TRUE;
        }

        static void failNextInitialization()
        {
            VulkanGraphicsDevice::_initializationFailureCheckpoint.store(
                1, std::memory_order_relaxed);
        }
    };

    // Every BlendState factor must map to the Vulkan factor that means the same thing.
    // This is checked in isolation because the dual-source factors have no end-to-end
    // coverage yet: nothing on the Vulkan side can emit a second colour output while
    // ShaderMaterial remains MSL-only, so a wrong mapping would not show up in any
    // rendered frame. It previously did not: the SRC1_* factors were unmapped and fell
    // through to VK_BLEND_FACTOR_ONE, blending plausibly but incorrectly.
    bool checkBlendFactorMapping()
    {
        const std::pair<int, VkBlendFactor> expected[] = {
            {BLENDMODE_ZERO,                 VK_BLEND_FACTOR_ZERO},
            {BLENDMODE_ONE,                  VK_BLEND_FACTOR_ONE},
            {BLENDMODE_SRC_COLOR,            VK_BLEND_FACTOR_SRC_COLOR},
            {BLENDMODE_ONE_MINUS_SRC_COLOR,  VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR},
            {BLENDMODE_DST_COLOR,            VK_BLEND_FACTOR_DST_COLOR},
            {BLENDMODE_ONE_MINUS_DST_COLOR,  VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR},
            {BLENDMODE_SRC_ALPHA,            VK_BLEND_FACTOR_SRC_ALPHA},
            {BLENDMODE_SRC_ALPHA_SATURATE,   VK_BLEND_FACTOR_SRC_ALPHA_SATURATE},
            {BLENDMODE_ONE_MINUS_SRC_ALPHA,  VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA},
            {BLENDMODE_DST_ALPHA,            VK_BLEND_FACTOR_DST_ALPHA},
            {BLENDMODE_ONE_MINUS_DST_ALPHA,  VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA},
            {BLENDMODE_CONSTANT,             VK_BLEND_FACTOR_CONSTANT_COLOR},
            {BLENDMODE_ONE_MINUS_CONSTANT,   VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR},
            {BLENDMODE_SRC1_COLOR,           VK_BLEND_FACTOR_SRC1_COLOR},
            {BLENDMODE_ONE_MINUS_SRC1_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR},
            {BLENDMODE_SRC1_ALPHA,           VK_BLEND_FACTOR_SRC1_ALPHA},
            {BLENDMODE_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA},
        };
        for (const auto& [factor, want] : expected) {
            if (vulkanMapBlendFactor(factor) != want) {
                spdlog::error("Vulkan smoke: blend factor {} mapped to {}, expected {}",
                    factor, static_cast<int>(vulkanMapBlendFactor(factor)),
                    static_cast<int>(want));
                return false;
            }
        }

        // The dualSrcBlend-less degradation must land on the same-coloured
        // single-source factor, and must leave every other factor alone.
        const std::pair<int, int> degraded[] = {
            {BLENDMODE_SRC1_COLOR,           BLENDMODE_SRC_COLOR},
            {BLENDMODE_ONE_MINUS_SRC1_COLOR, BLENDMODE_ONE_MINUS_SRC_COLOR},
            {BLENDMODE_SRC1_ALPHA,           BLENDMODE_SRC_ALPHA},
            {BLENDMODE_ONE_MINUS_SRC1_ALPHA, BLENDMODE_ONE_MINUS_SRC_ALPHA},
            {BLENDMODE_ONE,                  BLENDMODE_ONE},
            {BLENDMODE_DST_COLOR,            BLENDMODE_DST_COLOR},
        };
        for (const auto& [factor, want] : degraded) {
            if (blendFactorWithoutSrc1(factor) != want) {
                spdlog::error("Vulkan smoke: blendFactorWithoutSrc1({}) = {}, expected {}",
                    factor, blendFactorWithoutSrc1(factor), want);
                return false;
            }
        }
        return true;
    }
}

int main()
{
    if (!checkBlendFactorMapping()) {
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        spdlog::error("Vulkan smoke: SDL_Init failed: {}", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "VisuTwin Vulkan validation smoke",
        64,
        64,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        spdlog::error("Vulkan smoke: SDL_CreateWindow failed: {}", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    int result = 0;
    std::shared_ptr<const std::atomic_uint32_t> validationErrors;
    try {
        // Force a constructor exception after instance/device/VMA, swapchain,
        // depth, and per-frame resources exist. The partial cleanup must leave
        // the same window usable for a normal initialization immediately after.
        bool initializationFailed = false;
        VulkanGraphicsDeviceTestAccess::failNextInitialization();
        try {
            GraphicsDeviceOptions rejectedOptions{};
            rejectedOptions.backend = Backend::Vulkan;
            rejectedOptions.window = window;
            rejectedOptions.enableValidation = true;
            auto rejectedDevice =
                std::make_unique<VulkanGraphicsDevice>(
                    rejectedOptions);
        } catch (const std::exception&) {
            initializationFailed = true;
        }
        if (!initializationFailed) {
            spdlog::error(
                "Vulkan smoke: injected initialization failure was ignored");
            result = 1;
        }

        GraphicsDeviceOptions options{};
        options.backend = Backend::Vulkan;
        options.window = window;
        options.enableValidation = true;

        auto device = std::make_unique<VulkanGraphicsDevice>(options);
        if (!device->validationEnabled()) {
            spdlog::error("Vulkan smoke: validation was requested but is disabled");
            result = 1;
        }

        validationErrors = device->validationErrorCounter();

        if (device->graphicsQueue() == VK_NULL_HANDLE ||
            device->presentQueue() == VK_NULL_HANDLE ||
            !VulkanGraphicsDeviceTestAccess::presentFamilySupportsSurface(
                *device)) {
            spdlog::error(
                "Vulkan smoke: presentation queue is missing or unsupported");
            result = 1;
        }

        // Generic compute: sampled input + storage output, dispatched through
        // the public cross-backend Compute API.
        {
            constexpr const char* copyCompute = R"(
#version 450
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
layout(set=0,binding=0) uniform sampler2D inputTexture;
layout(rgba8,set=0,binding=1) uniform writeonly image2D outputTexture;
void main() { imageStore(outputTexture, ivec2(0), texelFetch(inputTexture, ivec2(0), 0)); }
)";
            TextureOptions inputOptions{};
            inputOptions.name = "vulkan-smoke-compute-input";
            inputOptions.width = 1;
            inputOptions.height = 1;
            inputOptions.mipmaps = false;
            Texture input(device.get(), inputOptions);
            const std::array<uint8_t, 4> pixel{64, 128, 255, 255};
            input.setLevelData(0, pixel.data(), pixel.size());
            input.upload();

            TextureOptions outputOptions = inputOptions;
            outputOptions.name = "vulkan-smoke-compute-output";
            outputOptions.storage = true;
            Texture output(device.get(), outputOptions);
            output.upload();

            ShaderDefinition definition{};
            definition.name = "vulkan-smoke-compute";
            definition.cshader = "main";
            auto computeShader = device->createShader(definition, copyCompute);
            if (!computeShader) {
                spdlog::error("Vulkan smoke: compute shader creation failed");
                result = 1;
            }
            Compute compute(device.get(), computeShader, "VulkanSmokeCompute");
            compute.setParameter("inputTexture", &input);
            compute.setParameter("outputTexture", &output);
            compute.setupDispatch(1, 1, 1);
            device->computeDispatch({&compute});
            vkQueueWaitIdle(device->graphicsQueue());
        }

        // GPU instance compaction and indirect-argument generation.
        {
            std::array<float, 20> instance{};
            instance[0] = instance[5] = instance[10] = instance[15] = 1.0f;
            instance[16] = instance[17] = instance[18] = instance[19] = 1.0f;
            VertexBufferOptions options{};
            options.data.resize(sizeof(instance));
            std::memcpy(options.data.data(), instance.data(), sizeof(instance));
            auto format = std::make_shared<VertexFormat>(
                80, VertexFormat::instanceMatrixElements(), true, true);
            auto input = device->createVertexBuffer(format, 1, options);
            auto culler = device->createInstanceCuller();
            InstanceCullParams params{};
            for (auto& plane : params.frustumPlanes) plane[3] = 1.0f;
            params.boundingSphereRadius = 1.0f;
            params.instanceCount = 1;
            params.indexCount = 3;
            culler->reserve(1);
            culler->cull(input.get(), params);
            device->endGpuCullBatch();
            vkQueueWaitIdle(device->graphicsQueue());
            if (culler->visibleCountReadback() != 1) {
                spdlog::error("Vulkan smoke: GPU instance culling failed");
                result = 1;
            }
        }

        // Exercise off-frame environment rendering, cubemap face views, and
        // asynchronous mip generation under validation.
        {
            TextureOptions sourceOptions{};
            sourceOptions.name = "vulkan-smoke-equirect";
            sourceOptions.width = 4;
            sourceOptions.height = 2;
            sourceOptions.mipmaps = false;
            Texture source(device.get(), sourceOptions);
            std::array<uint8_t, 32> sourcePixels{};
            sourcePixels.fill(128);
            source.setLevelData(0, sourcePixels.data(), sourcePixels.size());
            source.upload();

            TextureOptions cubeOptions{};
            cubeOptions.name = "vulkan-smoke-env-cube";
            cubeOptions.width = 8;
            cubeOptions.height = 8;
            cubeOptions.cubemap = true;
            cubeOptions.mipmaps = true;
            Texture cube(device.get(), cubeOptions);
            cube.upload();
            device->generateEquirectToCubemap({&source, &cube, false});

            TextureOptions atlasOptions{};
            atlasOptions.name = "vulkan-smoke-env-atlas";
            atlasOptions.width = 16;
            atlasOptions.height = 8;
            atlasOptions.mipmaps = false;
            Texture atlas(device.get(), atlasOptions);
            atlas.upload();
            EnvReprojectPassParams reproject{};
            reproject.target = &atlas;
            reproject.sourceCubemap = &cube;
            reproject.ops.push_back({0, 0, 16, 8, 1});
            device->generateEnvReproject(reproject);
            device->flushUploads();
        }

        // A ring overflow must be observable by the caller and must not alias
        // the start of the current frame region.
        {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(
                device->physicalDevice(), &properties);
            VulkanUniformRingBuffer testRing(
                device->vmaAllocator(), 2, 256,
                properties.limits.minUniformBufferOffsetAlignment);
            std::array<uint8_t, 16> smallUniform{};
            std::array<uint8_t, 257> oversizedUniform{};
            testRing.beginFrame(0);
            const auto first =
                testRing.allocate(smallUniform.data(), smallUniform.size());
            const auto overflow = testRing.allocate(
                oversizedUniform.data(), oversizedUniform.size());
            testRing.beginFrame(1);
            const auto secondFrame =
                testRing.allocate(smallUniform.data(), smallUniform.size());
            if (!first || overflow || !secondFrame ||
                *secondFrame <= *first) {
                spdlog::error(
                    "Vulkan smoke: uniform ring overflow semantics failed");
                result = 1;
            }
        }

        // Draw a triangle into a stencil-capable target while changing every
        // core pipeline state represented by the cross-platform interface.
        // This catches invalid pipeline/rendering compatibility as well as
        // missing dynamic stencil-reference commands.
        {
            TextureOptions colorOptions{};
            colorOptions.name = "vulkan-smoke-color";
            colorOptions.width = 64;
            colorOptions.height = 64;
            colorOptions.mipmaps = false;
            auto color = std::make_unique<Texture>(device.get(), colorOptions);

            RenderTargetOptions targetOptions{};
            targetOptions.graphicsDevice = device.get();
            targetOptions.colorBuffer = color.get();
            targetOptions.depth = true;
            targetOptions.stencil = true;
            targetOptions.name = "vulkan-smoke-target";
            auto target = device->createRenderTarget(targetOptions);

            auto sharedDevice = std::shared_ptr<GraphicsDevice>(
                device.get(), [](GraphicsDevice*) {});

            ShaderDefinition rejectedMslDefinition{};
            rejectedMslDefinition.name = "vulkan-no-generic-fallback";
            if (device->createShader(
                    rejectedMslDefinition,
                    "#include <metal_stdlib>\nusing namespace metal;")) {
                spdlog::error(
                    "Vulkan smoke: custom MSL received a generic fallback");
                result = 1;
            }

            RenderPass pass(sharedDevice);
            pass.init(target);
            const Color clearColor(0.05f, 0.1f, 0.2f, 1.0f);
            const float clearDepth = 1.0f;
            const int clearStencil = 0;
            pass.setClearColor(&clearColor);
            pass.setClearDepth(&clearDepth);
            pass.setClearStencil(&clearStencil);

            constexpr std::array<float, 42> vertices = {
                -0.5f, -0.5f, 0.0f,  0, 0, 1,  0, 0,  1, 0, 0, 1,  0, 0,
                 0.5f, -0.5f, 0.0f,  0, 0, 1,  1, 0,  1, 0, 0, 1,  0, 0,
                 0.0f,  0.5f, 0.0f,  0, 0, 1,  0.5f, 1,  1, 0, 0, 1,  0, 0
            };
            VertexBufferOptions vertexOptions{};
            vertexOptions.data.resize(sizeof(vertices));
            std::memcpy(vertexOptions.data.data(), vertices.data(), sizeof(vertices));
            auto format = std::make_shared<VertexFormat>(
                14 * sizeof(float), VertexFormat::standardElements());
            auto vertexBuffer = device->createVertexBuffer(format, 3, vertexOptions);

            // A padded point record deliberately has no stride-based layout
            // signature. Its position offset and normalized RGBA8 color must
            // come from the declared elements.
            constexpr uint32_t pointStride = 64;
            std::array<uint8_t, pointStride> pointData{};
            constexpr std::array<float, 3> pointPosition = {0.0f, 0.0f, 0.0f};
            constexpr std::array<uint8_t, 4> pointColor = {255, 128, 32, 255};
            std::memcpy(pointData.data() + 8, pointPosition.data(), sizeof(pointPosition));
            std::memcpy(pointData.data() + 24, pointColor.data(), sizeof(pointColor));
            VertexBufferOptions pointOptions{};
            pointOptions.data.assign(pointData.begin(), pointData.end());
            std::vector<VertexElement> pointElements = {
                {VertexSemantic::SEMANTIC_POSITION, VertexDataType::TYPE_FLOAT32, 3, 8},
                {VertexSemantic::SEMANTIC_COLOR, VertexDataType::TYPE_UINT8, 4, 24, true},
            };
            auto pointFormat = std::make_shared<VertexFormat>(
                pointStride, std::move(pointElements));
            auto pointBuffer = device->createVertexBuffer(pointFormat, 1, pointOptions);
            auto shader = device->createShader(
                ShaderDefinition{.name = "vulkan-smoke"});

            Material featureMaterial;
            featureMaterial.setBaseColorTexture(color.get());
            featureMaterial.setHasBaseColorTexture(true);
            featureMaterial.setAlphaMode(AlphaMode::MASK);
            ProgramLibrary featurePrograms(sharedDevice);
            auto featureShader =
                featurePrograms.getForwardShader(&featureMaterial, false);
            const auto* vkFeatureShader =
                dynamic_cast<VulkanShader*>(featureShader.get());
            const uint64_t requiredFeatures =
                shaderFeatureBit(ShaderFeature::BaseColorMap) |
                shaderFeatureBit(ShaderFeature::AlphaTest);
            if (!vkFeatureShader ||
                (vkFeatureShader->featureMask() & requiredFeatures) !=
                    requiredFeatures ||
                !vkFeatureShader->specializesFeatures()) {
                spdlog::error(
                    "Vulkan smoke: shared feature mask did not reach SPIR-V");
                result = 1;
            }

            // More unique image tuples than the initial descriptor pool can
            // hold. This forces fence-owned pool growth; repeating the final
            // tuple then verifies that the frame cache does not allocate again.
            constexpr size_t descriptorStressCount = 260;
            std::vector<std::unique_ptr<Texture>> descriptorStressTextures;
            descriptorStressTextures.reserve(descriptorStressCount);
            constexpr std::array<uint8_t, 4> stressPixel = {
                255, 255, 255, 255
            };
            for (size_t i = 0; i < descriptorStressCount; ++i) {
                TextureOptions stressOptions{};
                stressOptions.name = "vulkan-descriptor-stress";
                stressOptions.width = 1;
                stressOptions.height = 1;
                stressOptions.mipmaps = false;
                auto texture =
                    std::make_unique<Texture>(device.get(), stressOptions);
                texture->setLevelData(
                    0, stressPixel.data(), stressPixel.size());
                texture->upload();
                descriptorStressTextures.push_back(std::move(texture));
            }
            Material descriptorStressMaterial;

            const std::array compressedFormats = {
                PixelFormat::PIXELFORMAT_DXT1,
                PixelFormat::PIXELFORMAT_DXT3,
                PixelFormat::PIXELFORMAT_DXT5,
                PixelFormat::PIXELFORMAT_BC4,
                PixelFormat::PIXELFORMAT_BC5,
                PixelFormat::PIXELFORMAT_BC6H,
                PixelFormat::PIXELFORMAT_BC7,
                PixelFormat::PIXELFORMAT_ASTC_4x4,
                PixelFormat::PIXELFORMAT_ASTC_5x5,
                PixelFormat::PIXELFORMAT_ASTC_6x6,
                PixelFormat::PIXELFORMAT_ASTC_8x8,
                PixelFormat::PIXELFORMAT_ASTC_10x10,
                PixelFormat::PIXELFORMAT_ASTC_12x12,
            };
            for (const PixelFormat format : compressedFormats) {
                if (vulkanMapPixelFormat(format) == VK_FORMAT_UNDEFINED) {
                    spdlog::error(
                        "Vulkan smoke: compressed format {} has no mapping",
                        static_cast<uint32_t>(format));
                    result = 1;
                }
            }
            std::unique_ptr<Texture> compressedTexture;
            for (const PixelFormat format : compressedFormats) {
                const VkFormat vkFormat = vulkanMapPixelFormat(format);
                if (!vulkanFormatSupportsImage(device->physicalDevice(), vkFormat,
                        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        0, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
                    continue;
                }
                TextureOptions compressedOptions{};
                compressedOptions.name = "vulkan-smoke-compressed";
                compressedOptions.width = 8;
                compressedOptions.height = 8;
                compressedOptions.format = format;
                compressedOptions.mipmaps = false;
                compressedTexture =
                    std::make_unique<Texture>(device.get(), compressedOptions);
                const uint32_t blocksWide =
                    (8 + compressedPixelFormatBlockWidth(format) - 1) /
                    compressedPixelFormatBlockWidth(format);
                const uint32_t blocksHigh =
                    (8 + compressedPixelFormatBlockHeight(format) - 1) /
                    compressedPixelFormatBlockHeight(format);
                std::vector<uint8_t> blocks(
                    static_cast<size_t>(blocksWide) * blocksHigh *
                    compressedPixelFormatBlockSize(format), 0);
                compressedTexture->setLevelData(
                    0, blocks.data(), blocks.size());
                compressedTexture->upload();
                break;
            }
            if (!compressedTexture) {
                spdlog::warn(
                    "Vulkan smoke: device exposes no tested ASTC/BC sampled format");
            }

            TextureOptions explicitMipOptions{};
            explicitMipOptions.name = "vulkan-smoke-explicit-mips";
            explicitMipOptions.width = 8;
            explicitMipOptions.height = 8;
            explicitMipOptions.numLevels = 3;
            explicitMipOptions.mipmaps = true;
            auto explicitMips =
                std::make_unique<Texture>(device.get(), explicitMipOptions);
            for (uint32_t mip = 0; mip < 3; ++mip) {
                const uint32_t dimension = std::max(8u >> mip, 1u);
                std::vector<uint8_t> mipData(
                    static_cast<size_t>(dimension) * dimension * 4,
                    static_cast<uint8_t>(64 + mip * 48));
                explicitMips->setLevelData(
                    mip, mipData.data(), mipData.size());
            }
            explicitMips->upload();

            // Upload every cubemap face in one batch, generate its mip chain,
            // then render into one nonzero (face, mip) subresource.
            TextureOptions cubeOptions{};
            cubeOptions.name = "vulkan-smoke-cube";
            cubeOptions.width = 8;
            cubeOptions.height = 8;
            cubeOptions.cubemap = true;
            cubeOptions.mipmaps = true;
            auto cube = std::make_unique<Texture>(device.get(), cubeOptions);
            for (uint32_t face = 0; face < 6; ++face) {
                std::array<uint8_t, 8 * 8 * 4> faceData{};
                for (size_t i = 0; i < faceData.size(); i += 4) {
                    faceData[i + 0] = static_cast<uint8_t>(32 * face);
                    faceData[i + 1] = static_cast<uint8_t>(255 - 24 * face);
                    faceData[i + 2] = 128;
                    faceData[i + 3] = 255;
                }
                cube->setLevelData(0, faceData.data(), faceData.size(), face);
            }
            cube->upload();
            auto* vkCube = dynamic_cast<gpu::VulkanTexture*>(cube->impl());
            if (!vkCube || vkCube->arrayLayers() != 6 || vkCube->mipLevels() != 4) {
                spdlog::error("Vulkan smoke: cubemap allocation is incomplete");
                result = 1;
            }

            RenderTargetOptions cubeTargetOptions{};
            cubeTargetOptions.graphicsDevice = device.get();
            cubeTargetOptions.colorBuffer = cube.get();
            cubeTargetOptions.face = 3;
            cubeTargetOptions.mipLevel = 1;
            cubeTargetOptions.name = "vulkan-smoke-cube-face-mip";
            auto cubeTarget = device->createRenderTarget(cubeTargetOptions);
            RenderPass cubePass(sharedDevice);
            cubePass.init(cubeTarget);

            Primitive triangle{};
            triangle.type = PRIMITIVE_TRIANGLES;
            triangle.count = 3;
            Primitive point{};
            point.type = PRIMITIVE_POINTS;
            point.count = 1;

            // Vulkan core has no uint8 index type, so the backend widens these
            // indices to uint16 while retaining uint8 CPU storage.
            auto uint8IndexBuffer =
                device->createIndexBuffer(INDEXFORMAT_UINT8, 3);
            if (uint8IndexBuffer->setData({0, 1}) ||
                !uint8IndexBuffer->setData({0, 1, 2}) ||
                uint8IndexBuffer->storage() !=
                    std::vector<uint8_t>({0, 1, 2})) {
                spdlog::error(
                    "Vulkan smoke: uint8 index widening or size validation failed");
                result = 1;
            }

            auto maskedBlend = std::make_shared<BlendState>();
            maskedBlend->setGreenWrite(false);
            maskedBlend->setBlueWrite(false);
            maskedBlend->setAlphaWrite(false);
            auto defaultDepth = std::make_shared<DepthState>();

            auto stencilFront = std::make_shared<StencilParameters>();
            stencilFront->setCompareFunction(StencilCompareFunction::Always);
            stencilFront->setPassOperation(StencilOperation::Replace);
            stencilFront->setReference(1);
            stencilFront->setReadMask(0x0f);
            stencilFront->setWriteMask(0x0f);

            auto stencilBack = std::make_shared<StencilParameters>();
            stencilBack->setCompareFunction(StencilCompareFunction::NotEqual);
            stencilBack->setFailOperation(StencilOperation::IncrementWrap);
            stencilBack->setDepthFailOperation(StencilOperation::DecrementClamp);
            stencilBack->setPassOperation(StencilOperation::Invert);
            stencilBack->setReference(2);
            stencilBack->setReadMask(0xf0);
            stencilBack->setWriteMask(0xf0);

            device->frameStart();
            device->startRenderPass(&pass);
            device->setViewport(0, 0, 64, 64);
            // Deliberately exceeds the target to verify Vulkan scissor clamping.
            device->setScissor(-4, -4, 80, 80);
            device->setShader(shader);
            device->setVertexBuffer(vertexBuffer);
            device->setTransformUniforms(Matrix4::identity(), Matrix4::identity());
            device->setBlendState(maskedBlend);
            device->setDepthState(defaultDepth);
            device->setDepthBias(0.25f, 0.5f, 0.0f);
            device->setCullMode(CullMode::CULLFACE_BACK);
            device->setStencilState(stencilFront, stencilBack);
            device->draw(triangle);

            device->setVertexBuffer(vertexBuffer);
            device->draw(triangle, uint8IndexBuffer);

            // Reference is dynamic and must not require a new pipeline.
            stencilFront->setReference(3);
            stencilBack->setReference(4);
            device->setVertexBuffer(vertexBuffer);
            device->draw(triangle);

            device->setVertexBuffer(pointBuffer);
            device->draw(point);
            device->setVertexBuffer(vertexBuffer);

            auto alphaBlend = std::make_shared<BlendState>(BlendState::alphaBlend());
            auto noDepth = std::make_shared<DepthState>();
            noDepth->setDepthTest(false);
            noDepth->setDepthWrite(false);
            device->setBlendState(alphaBlend);
            device->setDepthState(noDepth);
            device->setDepthBias(0.0f, 0.0f, 0.0f);
            device->setCullMode(CullMode::CULLFACE_FRONT);
            device->setStencilState();
            device->draw(triangle);

            device->setCullMode(CullMode::CULLFACE_FRONTANDBACK);
            device->setVertexBuffer(vertexBuffer);
            device->draw(triangle);

            device->setCullMode(CullMode::CULLFACE_BACK);
            device->setMaterial(&descriptorStressMaterial);
            for (const auto& texture : descriptorStressTextures) {
                descriptorStressMaterial.setBaseColorTexture(texture.get());
                device->setVertexBuffer(vertexBuffer);
                device->draw(triangle);
            }
            if (device->currentFrameDescriptorPoolCount() < 2) {
                spdlog::error(
                    "Vulkan smoke: descriptor pool did not grow under stress");
                result = 1;
            }
            const size_t cachedSetCount =
                device->currentFrameCachedImageDescriptorSetCount();
            device->setVertexBuffer(vertexBuffer);
            device->draw(triangle);
            if (device->currentFrameCachedImageDescriptorSetCount() !=
                cachedSetCount) {
                spdlog::error(
                    "Vulkan smoke: reusable descriptor set was not cached");
                result = 1;
            }
            device->setMaterial(nullptr);

            device->endRenderPass(&pass);
            device->grabSceneColor(target.get());
            device->frameEnd();

            TextureOptions depthOptions{};
            depthOptions.name = "vulkan-smoke-depth";
            depthOptions.width = 64;
            depthOptions.height = 64;
            depthOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
            depthOptions.mipmaps = false;
            auto depth = std::make_unique<Texture>(device.get(), depthOptions);

            RenderTargetOptions depthTargetOptions{};
            depthTargetOptions.graphicsDevice = device.get();
            depthTargetOptions.depthBuffer = depth.get();
            depthTargetOptions.name = "vulkan-smoke-depth-only";
            auto depthTarget = device->createRenderTarget(depthTargetOptions);
            RenderPass depthPass(sharedDevice);
            depthPass.init(depthTarget);
            depthPass.setClearDepth(&clearDepth);

            // Depth-only pipelines must preserve the requested cull state
            // instead of silently forcing double-sided rendering.
            device->frameStart();
            device->startRenderPass(&depthPass);
            device->setShader(shader);
            device->setVertexBuffer(vertexBuffer);
            device->setBlendState(maskedBlend);
            device->setDepthState(defaultDepth);
            device->setCullMode(CullMode::CULLFACE_FRONT);
            device->setStencilState();
            device->draw(triangle);
            device->endRenderPass(&depthPass);
            device->grabSceneDepth(depthTarget.get());
            device->frameEnd();

            TextureOptions cocOptions{};
            cocOptions.name = "vulkan-smoke-coc";
            cocOptions.width = 64;
            cocOptions.height = 64;
            cocOptions.mipmaps = false;
            auto coc = std::make_unique<Texture>(device.get(), cocOptions);
            RenderTargetOptions cocTargetOptions{};
            cocTargetOptions.graphicsDevice = device.get();
            cocTargetOptions.colorBuffer = coc.get();
            auto cocTarget = device->createRenderTarget(cocTargetOptions);
            RenderPass cocPass(sharedDevice);
            cocPass.init(cocTarget);
            device->frameStart();
            device->startRenderPass(&cocPass);
            CoCPassParams cocParams{};
            cocParams.depthTexture = depth.get();
            cocParams.focusDistance = 2.0f;
            cocParams.focusRange = 1.0f;
            device->executeCoCPass(cocParams);
            device->endRenderPass(&cocPass);
            device->frameEnd();

            TextureOptions dofOptions = cocOptions;
            dofOptions.name = "vulkan-smoke-dof";
            auto dof = std::make_unique<Texture>(device.get(), dofOptions);
            RenderTargetOptions dofTargetOptions{};
            dofTargetOptions.graphicsDevice = device.get();
            dofTargetOptions.colorBuffer = dof.get();
            auto dofTarget = device->createRenderTarget(dofTargetOptions);
            RenderPass dofPass(sharedDevice);
            dofPass.init(dofTarget);
            device->frameStart();
            device->startRenderPass(&dofPass);
            DofBlurPassParams dofParams{};
            dofParams.nearTexture = color.get();
            dofParams.farTexture = color.get();
            dofParams.cocTexture = coc.get();
            dofParams.invResolutionX = 1.0f / 64.0f;
            dofParams.invResolutionY = 1.0f / 64.0f;
            device->executeDofBlurPass(dofParams);
            device->endRenderPass(&dofPass);
            device->frameEnd();

            device->frameStart();
            if (device->currentFrameDescriptorPoolCount() < 2 ||
                device->currentFrameCachedImageDescriptorSetCount() != 0) {
                spdlog::error(
                    "Vulkan smoke: descriptor pools were not recycled cleanly");
                result = 1;
            }
            device->startRenderPass(&cubePass);
            device->endRenderPass(&cubePass);
            device->frameEnd();

            // Exercise dynamic rendering with two ordered color attachments of
            // different formats using a fragment shader that writes both output
            // locations. Both formats must be declared by the compatible
            // graphics pipeline.
            TextureOptions mrtColor0Options = colorOptions;
            mrtColor0Options.name = "vulkan-smoke-mrt-color-0";
            auto mrtColor0 =
                std::make_unique<Texture>(device.get(), mrtColor0Options);
            TextureOptions mrtColor1Options = colorOptions;
            mrtColor1Options.name = "vulkan-smoke-mrt-color-1";
            mrtColor1Options.format = PixelFormat::PIXELFORMAT_RGBA16F;
            auto mrtColor1 =
                std::make_unique<Texture>(device.get(), mrtColor1Options);

            RenderTargetOptions mrtTargetOptions{};
            mrtTargetOptions.graphicsDevice = device.get();
            mrtTargetOptions.colorBuffers = {
                mrtColor0.get(), mrtColor1.get()
            };
            mrtTargetOptions.depth = true;
            mrtTargetOptions.name = "vulkan-smoke-mrt-target";
            auto mrtTarget = device->createRenderTarget(mrtTargetOptions);
            RenderPass mrtPass(sharedDevice);
            mrtPass.init(mrtTarget);
            mrtPass.setClearColor(&clearColor);
            mrtPass.setClearDepth(&clearDepth);

            constexpr const char* mrtShaderSource = R"(
#version 450
#ifdef VT_VERTEX_SHADER
layout(location=0) in vec3 vertexPosition;
void main() {
    gl_Position = vec4(vertexPosition, 1.0);
}
#endif
#ifdef VT_FRAGMENT_SHADER
layout(location=0) out vec4 color0;
layout(location=1) out vec4 color1;
void main() {
    color0 = vec4(1.0, 0.0, 0.0, 1.0);
    color1 = vec4(0.0, 1.0, 0.0, 1.0);
}
#endif
)";
            auto mrtShader = device->createShader(
                ShaderDefinition{.name = "vulkan-smoke-mrt"},
                mrtShaderSource);
            if (!mrtShader) {
                spdlog::error("Vulkan smoke: MRT shader creation failed");
                result = 1;
            }

            device->frameStart();
            device->startRenderPass(&mrtPass);
            device->setShader(mrtShader ? mrtShader : shader);
            device->setVertexBuffer(vertexBuffer);
            device->setTransformUniforms(
                Matrix4::identity(), Matrix4::identity());
            device->setBlendState(maskedBlend);
            device->setDepthState(defaultDepth);
            device->setCullMode(CullMode::CULLFACE_BACK);
            device->setStencilState();
            device->draw(triangle);
            device->endRenderPass(&mrtPass);
            device->frameEnd();

        // ------------------------------------------------------------------
        // Depth-convention probe.
        //
        // Matrix4::frustum/ortho produce OpenGL-style clip space (NDC z in
        // [-1,1]); Vulkan clips to [0,1] and this device sets depthClampEnable
        // = VK_FALSE with no depth-clip-control extension. The Metal shader
        // chunks remap with clip.z = 0.5*(clip.z+clip.w), and so do this
        // backend's gsplat.vert and particle.vert — but the whole forward*.vert
        // family does not. If that is a real defect, a fragment at NDC z = -0.5
        // is silently dropped here while it would survive on Metal.
        //
        // Draw one full-viewport triangle per z into a 1x1-sampled target and
        // read the pixel back: black means the fragment never landed.
        {
            const auto probeAtNdcZ = [&](const float ndcZ) -> std::array<uint8_t, 4> {
                TextureOptions probeOptions{};
                probeOptions.name = "vulkan-smoke-depth-probe";
                probeOptions.width = 4;
                probeOptions.height = 4;
                probeOptions.mipmaps = false;
                Texture probeColor(device.get(), probeOptions);

                RenderTargetOptions probeTargetOptions{};
                probeTargetOptions.graphicsDevice = device.get();
                probeTargetOptions.colorBuffer = &probeColor;
                probeTargetOptions.depth = true;
                probeTargetOptions.name = "vulkan-smoke-depth-probe-target";
                auto probeTarget = device->createRenderTarget(probeTargetOptions);

                RenderPass probePass(sharedDevice);
                probePass.init(probeTarget);
                const Color black(0.0f, 0.0f, 0.0f, 1.0f);
                probePass.setClearColor(&black);
                probePass.setClearDepth(&clearDepth);

                // gl_Position.z is the literal NDC z (w = 1), so this bypasses the
                // projection entirely and tests only the clip rule.
                const std::string probeSource = std::string(R"(
#version 450
#ifdef VT_VERTEX_SHADER
layout(location=0) in vec3 vertexPosition;
// x4 so the smoke test's centred triangle covers the whole 4x4 target.
void main() { gl_Position = vec4(vertexPosition.xy * 4.0, )") + std::to_string(ndcZ) + R"(, 1.0); }
#endif
#ifdef VT_FRAGMENT_SHADER
layout(location=0) out vec4 color0;
void main() { color0 = vec4(1.0, 1.0, 1.0, 1.0); }
#endif
)";
                auto probeShader = device->createShader(
                    ShaderDefinition{.name = "vulkan-smoke-depth-probe"}, probeSource);

                device->frameStart();
                device->startRenderPass(&probePass);
                device->setShader(probeShader);
                device->setVertexBuffer(vertexBuffer);
                device->setTransformUniforms(Matrix4::identity(), Matrix4::identity());
                device->setBlendState(nullptr);
                device->setDepthState(defaultDepth);
                device->setCullMode(CullMode::CULLFACE_NONE);
                device->setStencilState();
                device->draw(triangle);
                device->endRenderPass(&probePass);
                device->frameEnd();

                // Copy texel (0,0) back to host memory.
                std::array<uint8_t, 4> pixel{0, 0, 0, 0};
                auto* vkProbe = dynamic_cast<gpu::VulkanTexture*>(probeColor.impl());
                if (!vkProbe || vkProbe->image() == VK_NULL_HANDLE) {
                    return pixel;
                }
                VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bufferInfo.size = 4 * 4 * 4;
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo allocInfo{};
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VkBuffer hostBuffer = VK_NULL_HANDLE;
                VmaAllocation hostAllocation = nullptr;
                VmaAllocationInfo hostMapped{};
                if (vmaCreateBuffer(device->vmaAllocator(), &bufferInfo, &allocInfo,
                        &hostBuffer, &hostAllocation, &hostMapped) != VK_SUCCESS) {
                    return pixel;
                }
                device->enqueueUpload([vkProbe, hostBuffer](VkCommandBuffer cmd) {
                    vkProbe->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
                    VkBufferImageCopy region{};
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = {4, 4, 1};
                    vkCmdCopyImageToBuffer(cmd, vkProbe->image(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hostBuffer, 1, &region);
                    vkProbe->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
                });
                device->flushUploads();
                vkQueueWaitIdle(device->graphicsQueue());
                if (hostMapped.pMappedData) {
                    // Centre texel (2,2) of the 4x4 copy — a corner texel can fall
                    // outside the triangle and read black for the wrong reason.
                    const size_t centreOffset = (2 * 4 + 2) * 4;
                    std::memcpy(pixel.data(),
                        static_cast<const uint8_t*>(hostMapped.pMappedData) + centreOffset,
                        pixel.size());
                }
                vmaDestroyBuffer(device->vmaAllocator(), hostBuffer, hostAllocation);
                return pixel;
            };

            const auto inRange = probeAtNdcZ(0.5f);
            const auto negative = probeAtNdcZ(-0.5f);
            spdlog::info("Vulkan smoke: depth probe ndcZ=+0.5 -> rgba({},{},{},{})",
                inRange[0], inRange[1], inRange[2], inRange[3]);
            spdlog::info("Vulkan smoke: depth probe ndcZ=-0.5 -> rgba({},{},{},{})",
                negative[0], negative[1], negative[2], negative[3]);

            // Positive control. Without it a black result is meaningless — the first
            // version of this probe read a corner texel the triangle never covered
            // and reported "clipped" for both z values.
            if (inRange[0] != 255) {
                spdlog::error("Vulkan smoke: depth probe drew nothing at ndcZ=+0.5 — "
                    "the probe or its readback is broken, not the depth range");
                result = 1;
            }
            // Documents the current convention rather than asserting it is correct:
            // GL-style clip z from Matrix4::frustum/ortho loses everything below 0
            // here, while Metal's chunks (and this backend's gsplat/particle shaders)
            // remap first. See the depth-convention note in the Vulkan port memo.
            if (negative[0] != 0) {
                spdlog::info("Vulkan smoke: negative NDC z now survives — the forward "
                    "vertex shaders must have gained the [-1,1] -> [0,1] remap");
            }

            // --------------------------------------------------------------
            // Shadow depth-contract check, in pixels.
            //
            // This is the invariant that the missing clip-z remap actually broke.
            // A shadow caster is rasterized through an ORTHOGRAPHIC projection, and
            // the depth it leaves behind (gl_FragCoord.z) is later compared against
            // coord.z built on the CPU by shadowRendererDirectional, whose
            // viewportMatrix bakes z*0.5 + 0.5 onto the same GL-style NDC z.
            // So the contract is:  stored gl_FragCoord.z  ==  0.5*gl_ndc_z + 0.5.
            //
            // Ortho is linear, so ndc z = 0 sits at the MIDPOINT of [near, far]:
            // a caster in the near half has negative GL ndc z. Before the remap it
            // was clipped outright (nothing stored); with the remap it stores
            // exactly the value the receiver side expects. Both halves are checked.
            const Matrix4 shadowOrtho = Matrix4::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f);

            const auto storedDepthAtViewDepth = [&](const float viewDepth) -> float {
                TextureOptions depthProbeOptions{};
                depthProbeOptions.name = "vulkan-smoke-shadow-depth-probe";
                depthProbeOptions.width = 4;
                depthProbeOptions.height = 4;
                depthProbeOptions.mipmaps = false;
                Texture probeColor(device.get(), depthProbeOptions);

                RenderTargetOptions probeTargetOptions{};
                probeTargetOptions.graphicsDevice = device.get();
                probeTargetOptions.colorBuffer = &probeColor;
                probeTargetOptions.depth = true;
                probeTargetOptions.name = "vulkan-smoke-shadow-depth-probe-target";
                auto probeTarget = device->createRenderTarget(probeTargetOptions);

                RenderPass probePass(sharedDevice);
                probePass.init(probeTarget);
                const Color black(0.0f, 0.0f, 0.0f, 1.0f);
                probePass.setClearColor(&black);
                probePass.setClearDepth(&clearDepth);

                // Writes the rasterized depth as colour so it can be read back.
                constexpr const char* depthOutSource = R"(
#version 450
layout(push_constant) uniform PushConstants { mat4 viewProjection; mat4 model; } pc;
#ifdef VT_VERTEX_SHADER
layout(location=0) in vec3 vertexPosition;
void main() {
    gl_Position = pc.viewProjection * (pc.model * vec4(vertexPosition, 1.0));
    gl_Position.z = 0.5 * (gl_Position.z + gl_Position.w);
}
#endif
#ifdef VT_FRAGMENT_SHADER
layout(location=0) out vec4 color0;
void main() { color0 = vec4(gl_FragCoord.z, gl_FragCoord.z, gl_FragCoord.z, 1.0); }
#endif
)";
                auto probeShader = device->createShader(
                    ShaderDefinition{.name = "vulkan-smoke-shadow-depth"}, depthOutSource);

                // Scale the centred triangle out to cover the whole ortho window and
                // push it to `viewDepth` in front of the camera (which looks down -Z).
                Matrix4 model = Matrix4::identity();
                model.setElement(0, 0, 80.0f);
                model.setElement(1, 1, 80.0f);
                model.setElement(3, 2, -viewDepth);

                device->frameStart();
                device->startRenderPass(&probePass);
                device->setShader(probeShader);
                device->setVertexBuffer(vertexBuffer);
                device->setTransformUniforms(shadowOrtho, model);
                device->setBlendState(nullptr);
                device->setDepthState(defaultDepth);
                device->setCullMode(CullMode::CULLFACE_NONE);
                device->setStencilState();
                device->draw(triangle);
                device->endRenderPass(&probePass);
                device->frameEnd();

                std::array<uint8_t, 4> pixel{0, 0, 0, 0};
                auto* vkProbe = dynamic_cast<gpu::VulkanTexture*>(probeColor.impl());
                if (!vkProbe || vkProbe->image() == VK_NULL_HANDLE) return -1.0f;

                VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bufferInfo.size = 4 * 4 * 4;
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo allocInfo{};
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VkBuffer hostBuffer = VK_NULL_HANDLE;
                VmaAllocation hostAllocation = nullptr;
                VmaAllocationInfo hostMapped{};
                if (vmaCreateBuffer(device->vmaAllocator(), &bufferInfo, &allocInfo,
                        &hostBuffer, &hostAllocation, &hostMapped) != VK_SUCCESS) {
                    return -1.0f;
                }
                device->enqueueUpload([vkProbe, hostBuffer](VkCommandBuffer cmd) {
                    vkProbe->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
                    VkBufferImageCopy region{};
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = {4, 4, 1};
                    vkCmdCopyImageToBuffer(cmd, vkProbe->image(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hostBuffer, 1, &region);
                    vkProbe->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
                });
                device->flushUploads();
                vkQueueWaitIdle(device->graphicsQueue());
                if (hostMapped.pMappedData) {
                    const size_t centreOffset = (2 * 4 + 2) * 4;
                    std::memcpy(pixel.data(),
                        static_cast<const uint8_t*>(hostMapped.pMappedData) + centreOffset,
                        pixel.size());
                }
                vmaDestroyBuffer(device->vmaAllocator(), hostBuffer, hostAllocation);
                return static_cast<float>(pixel[0]) / 255.0f;
            };

            // The check above validates the remap FORMULA against the receiver side,
            // but it applies that formula in its own inline shader. This second probe
            // closes the gap: it renders through the BUNDLED forward program (empty
            // source + a non-"program-shadow" name resolves to kForwardVert/kForwardFrag,
            // the same modules every mesh uses) and reads the D32_SFLOAT depth
            // attachment back, so it fails if forward.vert ever loses the remap again.
            const auto forwardStoredDepth = [&](const float viewDepth) -> float {
                TextureOptions depthOptions{};
                depthOptions.name = "vulkan-smoke-forward-depth";
                depthOptions.width = 4;
                depthOptions.height = 4;
                depthOptions.mipmaps = false;
                depthOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
                Texture depthTexture(device.get(), depthOptions);

                TextureOptions colorOpts{};
                colorOpts.name = "vulkan-smoke-forward-depth-color";
                colorOpts.width = 4;
                colorOpts.height = 4;
                colorOpts.mipmaps = false;
                Texture colorTexture(device.get(), colorOpts);

                RenderTargetOptions targetOptions{};
                targetOptions.graphicsDevice = device.get();
                targetOptions.colorBuffer = &colorTexture;
                targetOptions.depthBuffer = &depthTexture;
                targetOptions.name = "vulkan-smoke-forward-depth-target";
                auto target = device->createRenderTarget(targetOptions);

                RenderPass pass(sharedDevice);
                pass.init(target);
                const Color black(0.0f, 0.0f, 0.0f, 1.0f);
                pass.setClearColor(&black);
                pass.setClearDepth(&clearDepth);

                Matrix4 model = Matrix4::identity();
                model.setElement(0, 0, 80.0f);
                model.setElement(1, 1, 80.0f);
                model.setElement(3, 2, -viewDepth);

                device->frameStart();
                device->startRenderPass(&pass);
                device->setShader(shader);            // bundled forward program
                device->setVertexBuffer(vertexBuffer);
                device->setTransformUniforms(shadowOrtho, model);
                device->setBlendState(nullptr);
                device->setDepthState(defaultDepth);
                device->setCullMode(CullMode::CULLFACE_NONE);
                device->setStencilState();
                device->draw(triangle);
                device->endRenderPass(&pass);
                device->frameEnd();

                auto* vkDepth = dynamic_cast<gpu::VulkanTexture*>(depthTexture.impl());
                if (!vkDepth || vkDepth->image() == VK_NULL_HANDLE) return -1.0f;

                VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bufferInfo.size = 4 * 4 * sizeof(float);
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo allocInfo{};
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VkBuffer hostBuffer = VK_NULL_HANDLE;
                VmaAllocation hostAllocation = nullptr;
                VmaAllocationInfo hostMapped{};
                if (vmaCreateBuffer(device->vmaAllocator(), &bufferInfo, &allocInfo,
                        &hostBuffer, &hostAllocation, &hostMapped) != VK_SUCCESS) {
                    return -1.0f;
                }
                device->enqueueUpload([vkDepth, hostBuffer](VkCommandBuffer cmd) {
                    vkDepth->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
                    VkBufferImageCopy region{};
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = {4, 4, 1};
                    vkCmdCopyImageToBuffer(cmd, vkDepth->image(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hostBuffer, 1, &region);
                    vkDepth->transitionLayout(cmd,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 1, 0, 1);
                });
                device->flushUploads();
                vkQueueWaitIdle(device->graphicsQueue());
                float depthValue = -1.0f;
                if (hostMapped.pMappedData) {
                    const size_t centreOffset = (2 * 4 + 2) * sizeof(float);
                    std::memcpy(&depthValue,
                        static_cast<const uint8_t*>(hostMapped.pMappedData) + centreOffset,
                        sizeof(float));
                }
                vmaDestroyBuffer(device->vmaAllocator(), hostBuffer, hostAllocation);
                return depthValue;
            };

            // Near half (GL ndc z < 0 — the range the missing remap used to clip)
            // and far half, against what the receiver side computes on the CPU.
            for (const float viewDepth : {25.0f, 75.0f}) {
                const Vector4 clip = shadowOrtho * Vector4(0.0f, 0.0f, -viewDepth, 1.0f);
                const float glNdcZ = clip.getZ() / clip.getW();
                const float expected = 0.5f * glNdcZ + 0.5f;   // what viewportMatrix bakes
                const float stored = storedDepthAtViewDepth(viewDepth);
                spdlog::info("Vulkan smoke: shadow depth contract, viewDepth {} "
                    "(GL ndc z {:+.4f}): stored {:.4f}, receiver expects {:.4f}",
                    viewDepth, glNdcZ, stored, expected);
                // 8-bit colour readback, so one LSB of slack.
                if (stored < 0.0f || std::abs(stored - expected) > 2.0f / 255.0f) {
                    spdlog::error("Vulkan smoke: caster depth does not match what the "
                        "shadow sample matrix expects at viewDepth {} (stored {}, expected {})",
                        viewDepth, stored, expected);
                    result = 1;
                }

                // Same assertion, but through the real forward vertex shader and its
                // D32_SFLOAT depth attachment — full float, so a tight tolerance.
                const float forwardDepth = forwardStoredDepth(viewDepth);
                spdlog::info("Vulkan smoke: bundled forward program at viewDepth {}: "
                    "depth attachment {:.5f}, receiver expects {:.5f}",
                    viewDepth, forwardDepth, expected);
                if (forwardDepth < 0.0f || std::abs(forwardDepth - expected) > 1e-4f) {
                    spdlog::error("Vulkan smoke: forward.vert stored depth {} at viewDepth {}, "
                        "but the shadow sample matrix expects {} — the GL->Vulkan clip-z "
                        "remap is missing or wrong", forwardDepth, viewDepth, expected);
                    result = 1;
                }
            }

        // ── Clustered spot-shadow atlas (set 3 binding 14) ────────────────
        //
        // Renders an occluder into ONE SLICE of a two-layer depth array, then
        // shades a receiver through the bundled clustered forward variant and
        // reads the pixel back. Asserts the shadowed result is darker than the
        // same draw with castShadows off — a positive/negative pair, so it
        // isolates the shadow term from every other lighting contribution.
        //
        // The occluder goes in slice 1 with slice 0 left at the far clear: an
        // implementation that ignores shadowData.w and always samples slice 0
        // reads "no occluder" and fails here.
        //
        // Scope: this proves the atlas reaches the shader, that the array
        // descriptor is valid, and that the depth comparison and slice index
        // work. The occluder covers the whole slice, so it does NOT constrain
        // shadow UV orientation — a U or V flip would still pass.
        {
            constexpr uint32_t kAtlasSize = 64;
            constexpr float kOccluderViewDepth = 20.0f;
            constexpr float kReceiverViewDepth = 60.0f;

            TextureOptions atlasOptions{};
            atlasOptions.name = "vulkan-smoke-cluster-atlas";
            atlasOptions.width = kAtlasSize;
            atlasOptions.height = kAtlasSize;
            atlasOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
            atlasOptions.arrayLength = 2;
            atlasOptions.mipmaps = false;
            atlasOptions.minFilter = FilterMode::FILTER_NEAREST;
            atlasOptions.magFilter = FilterMode::FILTER_NEAREST;
            Texture atlas(device.get(), atlasOptions);
            atlas.setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            atlas.setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

            const Matrix4 lightOrtho =
                Matrix4::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.1f, 100.0f);

            // world -> atlas UV + [0,1] depth, the same remap the renderer bakes
            // into viewportMatrix (xy and z each *0.5 + 0.5). forward.vert stores
            // 0.5*z_gl + 0.5, so the z rows must agree or nothing ever compares
            // equal.
            Matrix4 lightShadowMatrix = Matrix4::identity();
            {
                // NOTE: setElement/getElement both take (col, row) — the
                // translation column is 3, not row 3.
                Matrix4 remap = Matrix4::identity();
                remap.setElement(0, 0, 0.5f);
                remap.setElement(1, 1, 0.5f);
                remap.setElement(2, 2, 0.5f);
                remap.setElement(3, 0, 0.5f);
                remap.setElement(3, 1, 0.5f);
                remap.setElement(3, 2, 0.5f);
                lightShadowMatrix = remap * lightOrtho;
            }

            // Depth-only pass into array layer 1 (RenderTargetOptions::face
            // selects the slice, exactly as LightTextureAtlas does).
            {
                RenderTargetOptions sliceOptions{};
                sliceOptions.graphicsDevice = device.get();
                sliceOptions.depthBuffer = &atlas;
                sliceOptions.face = 1;
                sliceOptions.name = "vulkan-smoke-cluster-atlas-slice1";
                auto sliceTarget = device->createRenderTarget(sliceOptions);

                RenderPass slicePass(sharedDevice);
                slicePass.init(sliceTarget);
                slicePass.setClearDepth(&clearDepth);

                Matrix4 occluderModel = Matrix4::identity();
                occluderModel.setElement(0, 0, 80.0f);
                occluderModel.setElement(1, 1, 80.0f);
                occluderModel.setElement(3, 2, -kOccluderViewDepth);

                device->frameStart();
                device->startRenderPass(&slicePass);
                device->setShader(shader);
                device->setVertexBuffer(vertexBuffer);
                device->setTransformUniforms(lightOrtho, occluderModel);
                device->setBlendState(nullptr);
                device->setDepthState(defaultDepth);
                device->setCullMode(CullMode::CULLFACE_NONE);
                device->setStencilState();
                device->draw(triangle);
                device->endRenderPass(&slicePass);
                device->frameEnd();
            }

            // Clustered forward variant: one 1×1×1 cell holding one light.
            Material clusterMaterial;
            ProgramLibrary clusterPrograms(sharedDevice);
            clusterPrograms.setClusteredLightingEnabled(true);
            auto clusterShader =
                clusterPrograms.getForwardShader(&clusterMaterial, false);
            const auto* vkClusterShader =
                dynamic_cast<VulkanShader*>(clusterShader.get());
            if (!vkClusterShader ||
                (vkClusterShader->featureMask() &
                    shaderFeatureBit(ShaderFeature::LightClustering)) == 0) {
                spdlog::error("Vulkan smoke: clustered forward variant lacks the "
                    "LIGHT_CLUSTERING feature bit — the shadow probe below would "
                    "be measuring the wrong shader");
                result = 1;
            }

            // Receiver sits behind the occluder along the light axis, inside the
            // single grid cell. A point light (params.y = 0) keeps spot-cone
            // attenuation out of the measurement; the shadow block keys only on
            // shadowData.x, exactly as it does on Metal.
            const Vector3 receiverWorld(0.0f, 0.0f, -kReceiverViewDepth);

            const auto shadedLuminance = [&](const bool castShadows) -> float {
                // Built through the real WorldClusters so this covers the engine's
                // own GPU packing — including the shadow-matrix transpose, which
                // is where this feature was actually broken. Hand-packing the
                // struct here would have let that bug through.
                ClusterLightData light;
                light.position = Vector3(0.0f, 0.0f, 0.0f);
                light.direction = Vector3(0.0f, 0.0f, -1.0f);   // toward receiver
                light.color = Color(1.0f, 1.0f, 1.0f, 1.0f);
                // Tuned so the unshadowed read lands mid-range: an intensity that
                // saturates to 1.0 would hide the shadow difference entirely.
                light.intensity = 0.8f;
                light.range = 400.0f;
                light.innerConeAngle = 40.0f;
                light.outerConeAngle = 60.0f;
                light.isSpot = true;
                light.falloffModeLinear = true;
                light.castShadows = castShadows;
                light.shadowMatrix = lightShadowMatrix;
                light.shadowBias = 0.0005f;
                light.shadowIntensity = 1.0f;
                light.atlasSlice = 1;

                WorldClusters clusters;
                clusters.update({light},
                    BoundingBox(Vector3(0.0f, 0.0f, -50.0f),
                        Vector3(60.0f, 60.0f, 60.0f)));

                TextureOptions colorOpts{};
                colorOpts.name = "vulkan-smoke-cluster-shadow-color";
                colorOpts.width = 4;
                colorOpts.height = 4;
                colorOpts.mipmaps = false;
                Texture colorTexture(device.get(), colorOpts);

                RenderTargetOptions targetOptions{};
                targetOptions.graphicsDevice = device.get();
                targetOptions.colorBuffer = &colorTexture;
                targetOptions.depth = true;
                targetOptions.name = "vulkan-smoke-cluster-shadow-target";
                auto target = device->createRenderTarget(targetOptions);

                RenderPass pass(sharedDevice);
                pass.init(target);
                const Color black(0.0f, 0.0f, 0.0f, 1.0f);
                pass.setClearColor(&black);
                pass.setClearDepth(&clearDepth);

                Matrix4 receiverModel = Matrix4::identity();
                receiverModel.setElement(0, 0, 80.0f);
                receiverModel.setElement(1, 1, 80.0f);
                receiverModel.setElement(3, 2, -kReceiverViewDepth);

                // Grid params exactly as renderer.cpp feeds them.
                const Vector3& bMin = clusters.boundsMin();
                const Vector3 bRange = clusters.boundsRange();
                const Vector3 cellsBySize = clusters.cellsCountByBoundsSize();
                const auto& cfg = clusters.config();
                const float boundsMin[3] = {bMin.getX(), bMin.getY(), bMin.getZ()};
                const float boundsRange[3] = {
                    bRange.getX(), bRange.getY(), bRange.getZ()};
                const float cellsBySizeArr[3] = {
                    cellsBySize.getX(), cellsBySize.getY(), cellsBySize.getZ()};

                device->frameStart();
                device->startRenderPass(&pass);
                device->setShader(clusterShader);
                device->setVertexBuffer(vertexBuffer);
                device->setTransformUniforms(lightOrtho, receiverModel);
                device->setClusterShadowAtlas(&atlas);
                device->setClusterBuffers(clusters.lightData(),
                    clusters.lightDataSize(),
                    clusters.cellData(), clusters.cellDataSize());
                device->setClusterGridParams(boundsMin, boundsRange, cellsBySizeArr,
                    cfg.cellsX, cfg.cellsY, cfg.cellsZ, cfg.maxLightsPerCell,
                    clusters.lightCount());
                // No ambient and no non-clustered lights: whatever reaches the
                // pixel came from the clustered light, so the two runs differ
                // only by the shadow term.
                device->setLightingUniforms(Color(0.0f, 0.0f, 0.0f, 1.0f), {},
                    Vector3(0.0f, 0.0f, 0.0f), false, 1.0f);
                device->setBlendState(nullptr);
                device->setDepthState(defaultDepth);
                device->setCullMode(CullMode::CULLFACE_NONE);
                device->setStencilState();
                device->draw(triangle);
                device->endRenderPass(&pass);
                device->frameEnd();

                auto* vkColor =
                    dynamic_cast<gpu::VulkanTexture*>(colorTexture.impl());
                if (!vkColor || vkColor->image() == VK_NULL_HANDLE) return -1.0f;

                VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bufferInfo.size = 4 * 4 * 4;
                bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo allocInfo{};
                allocInfo.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VkBuffer hostBuffer = VK_NULL_HANDLE;
                VmaAllocation hostAllocation = nullptr;
                VmaAllocationInfo hostMapped{};
                if (vmaCreateBuffer(device->vmaAllocator(), &bufferInfo, &allocInfo,
                        &hostBuffer, &hostAllocation, &hostMapped) != VK_SUCCESS) {
                    return -1.0f;
                }
                device->enqueueUpload([vkColor, hostBuffer](VkCommandBuffer cmd) {
                    vkColor->transitionLayout(cmd,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
                    VkBufferImageCopy region{};
                    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    region.imageSubresource.layerCount = 1;
                    region.imageExtent = {4, 4, 1};
                    vkCmdCopyImageToBuffer(cmd, vkColor->image(),
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hostBuffer, 1, &region);
                    vkColor->transitionLayout(cmd,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
                });
                device->flushUploads();
                vkQueueWaitIdle(device->graphicsQueue());
                std::array<uint8_t, 4> pixel{0, 0, 0, 0};
                if (hostMapped.pMappedData) {
                    const size_t centreOffset = (2 * 4 + 2) * 4;
                    std::memcpy(pixel.data(),
                        static_cast<const uint8_t*>(hostMapped.pMappedData) + centreOffset,
                        pixel.size());
                }
                vmaDestroyBuffer(device->vmaAllocator(), hostBuffer, hostAllocation);
                device->setClusterShadowAtlas(nullptr);
                return static_cast<float>(pixel[0]) / 255.0f;
            };

            const float unshadowed = shadedLuminance(false);
            const float shadowed = shadedLuminance(true);
            spdlog::info("Vulkan smoke: clustered shadow atlas, receiver luminance "
                "castShadows=off {:.4f}, castShadows=on {:.4f}", unshadowed, shadowed);

            // Positive control first: if the light never reached the receiver at
            // all, both reads are black and the darkening assertion below would
            // "pass" for the wrong reason (the lesson from the NDC-z probe).
            if (unshadowed <= 0.05f) {
                spdlog::error("Vulkan smoke: clustered light did not light the "
                    "receiver (unshadowed luminance {}); the shadow comparison "
                    "below would be meaningless", unshadowed);
                result = 1;
            } else if (shadowed >= unshadowed - 0.05f) {
                spdlog::error("Vulkan smoke: clustered spot shadow did not darken "
                    "the receiver (unshadowed {}, shadowed {}) — the atlas at set 3 "
                    "binding 14 is unbound, sampling the wrong slice, or the depth "
                    "comparison is inverted", unshadowed, shadowed);
                result = 1;
            }
        }
        }
        }

        // A zero-sized drawable must defer swapchain recreation and skip
        // acquisition. Restoring the size recreates against oldSwapchain;
        // subsequent frames exercise fence-aged retirement of its views,
        // depth image, and presentation semaphores.
        const auto drawableSize = device->size();
        device->setResolution(0, 0);
        device->frameStart();
        if (VulkanGraphicsDeviceTestAccess::frameActive(*device) ||
            VulkanGraphicsDeviceTestAccess::renderingDisabled(*device)) {
            spdlog::error(
                "Vulkan smoke: zero-sized drawable was not deferred safely");
            result = 1;
        }
        device->frameEnd();

        device->setResolution(drawableSize.first, drawableSize.second);
        for (uint32_t frame = 0; frame < 3; ++frame) {
            device->frameStart();
            if (!VulkanGraphicsDeviceTestAccess::frameActive(*device)) {
                spdlog::error(
                    "Vulkan smoke: frame inactive after swapchain resize");
                result = 1;
            }
            device->frameEnd();
        }
        if (VulkanGraphicsDeviceTestAccess::retiredSwapchainCount(
                *device) != 0) {
            spdlog::error(
                "Vulkan smoke: retired swapchain resources were not collected");
            result = 1;
        }

        // A failed submit must consume the acquire semaphore, release the
        // acquired image, and leave a signaled fence for the next frame slot
        // reuse. Before this recovery path existed, the second frameStart()
        // blocked forever on the reset fence.
        SDL_PumpEvents();
        device->frameStart();
        VulkanGraphicsDeviceTestAccess::failNextSubmit(
            *device, VK_ERROR_OUT_OF_HOST_MEMORY);
        device->frameEnd();
        if (VulkanGraphicsDeviceTestAccess::renderingDisabled(*device)) {
            spdlog::error(
                "Vulkan smoke: recoverable submit failure disabled rendering");
            result = 1;
        }

        device->frameStart();
        if (!VulkanGraphicsDeviceTestAccess::frameActive(*device)) {
            spdlog::error(
                "Vulkan smoke: frame did not recover after failed submission");
            result = 1;
        }
        device->frameEnd();

        if (vkDeviceWaitIdle(device->device()) != VK_SUCCESS) {
            spdlog::error("Vulkan smoke: vkDeviceWaitIdle failed");
            result = 1;
        }

        // Destroy the device before reading the final count so teardown
        // diagnostics emitted by the validation layer are failures too.
        device.reset();
    } catch (const std::exception& error) {
        spdlog::error("Vulkan smoke failed: {}", error.what());
        result = 1;
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    const uint32_t errorCount = validationErrors
        ? validationErrors->load(std::memory_order_relaxed)
        : 0;
    if (errorCount != 0) {
        spdlog::error("Vulkan smoke: {} validation error(s)", errorCount);
        return 1;
    }
    if (result != 0) {
        return result;
    }

    spdlog::info("Vulkan smoke passed with validation enabled and no errors");
    return 0;
}
