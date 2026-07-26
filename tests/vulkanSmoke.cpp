// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
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
#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "scene/shader-lib/programLibrary.h"
#include "spdlog/spdlog.h"

using namespace visutwin::canvas;

int main()
{
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
        }

        for (int frame = 0; frame < 1 && result == 0; ++frame) {
            SDL_PumpEvents();
            device->frameStart();
            device->frameEnd();
        }

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
