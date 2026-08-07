// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#ifdef VISUTWIN_HAS_VULKAN

#define VMA_IMPLEMENTATION
#include "vulkanGraphicsDevice.h"

#include <algorithm>
#include <cstring>
#include <VkBootstrap.h>
#include <SDL3/SDL_vulkan.h>

#include "vulkanIndexBuffer.h"
#include "vulkanInstanceCullPass.h"
#include "vulkanRenderPipeline.h"
#include "vulkanRenderTarget.h"
#include "vulkanShader.h"
#include "vulkanShaderCompiler.h"
#include "vulkanTexture.h"
#include "vulkanUniformRingBuffer.h"
#include "vulkanUtils.h"
#include "vulkanVertexBuffer.h"

#include "core/math/color.h"
#include "core/math/vector3.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/shaderFeatures.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"

#include "vulkan/vulkan_shader_bundle.h"

namespace visutwin::canvas
{

    namespace
    {
        bool looksLikeGlsl(const std::string& source)
        {
            const auto firstChar = source.find_first_not_of(" \t\r\n");
            return firstChar != std::string::npos &&
                source.compare(firstChar, 8, "#version") == 0;
        }

        VKAPI_ATTR VkBool32 VKAPI_CALL vulkanValidationCallback(
            const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            const VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
            void* userData)
        {
            const char* message = callbackData && callbackData->pMessage
                ? callbackData->pMessage
                : "Vulkan validation emitted an empty message";

            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                if (userData) {
                    static_cast<std::atomic_uint32_t*>(userData)->fetch_add(
                        1, std::memory_order_relaxed);
                }
                spdlog::error("Vulkan validation: {}", message);
            } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
                spdlog::warn("Vulkan validation: {}", message);
            } else {
                spdlog::debug("Vulkan validation: {}", message);
            }
            return VK_FALSE;
        }
    }

    std::atomic_int
        VulkanGraphicsDevice::_initializationFailureCheckpoint{0};

    VulkanGraphicsDevice::VulkanGraphicsDevice(const GraphicsDeviceOptions& options)
    {
        try {
            initialize(options);
        } catch (...) {
            cleanupPartialInitialization();
            throw;
        }
    }

    void VulkanGraphicsDevice::initialize(
        const GraphicsDeviceOptions& options)
    {
        _window = options.window;
        _validationEnabled = options.enableValidation;
        if (_window == nullptr) {
            throw std::invalid_argument(
                "VulkanGraphicsDevice: window must not be null");
        }

        int w = 0, h = 0;
        if (!SDL_GetWindowSizeInPixels(_window, &w, &h)) {
            throw std::runtime_error(
                std::string(
                    "VulkanGraphicsDevice: failed to query window size in pixels: ") +
                SDL_GetError());
        }
        _width = w;
        _height = h;

        initInstance(_window);
        if (_instance == VK_NULL_HANDLE || _surface == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: instance/surface initialization failed");
        }

        initDevice();
        if (_physicalDevice == VK_NULL_HANDLE ||
            _device == VK_NULL_HANDLE ||
            _graphicsQueue == VK_NULL_HANDLE ||
            _presentQueue == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: device/queue initialization failed");
        }

        // VMA allocator
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = _physicalDevice;
        allocatorInfo.device = _device;
        allocatorInfo.instance = _instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        if (vmaCreateAllocator(
                &allocatorInfo, &_vmaAllocator) != VK_SUCCESS ||
            _vmaAllocator == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: VMA allocator creation failed");
        }

        if (!initSwapchain(_width, _height)) {
            throw std::runtime_error("VulkanGraphicsDevice: swapchain creation failed");
        }
        createDepthResources();
        createPerFrameResources();
        if (_initializationFailureCheckpoint.exchange(
                0, std::memory_order_relaxed) != 0) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: injected initialization failure");
        }

        // Upload command pool (batched, nonblocking staging transfers)
        VkCommandPoolCreateInfo uploadPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        uploadPoolInfo.queueFamilyIndex = _graphicsQueueFamily;
        if (vkCreateCommandPool(
                _device, &uploadPoolInfo, nullptr,
                &_uploadCommandPool) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: upload command pool creation failed");
        }

        // Render pipeline
        _renderPipeline = std::make_unique<VulkanRenderPipeline>(this);

        // Default sampler
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(
                _device, &samplerInfo, nullptr,
                &_defaultSampler) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: default sampler creation failed");
        }

        // Environment-atlas sampler: clamp-to-edge so the equirectangular seam
        // and the packed sub-rects (irradiance, roughness mips) never wrap
        // into each other.  Trilinear; no anisotropy (matches the Metal
        // envAtlasSampler rationale — anisotropy smears the atan2 wrap).
        VkSamplerCreateInfo envSamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        envSamplerInfo.magFilter = VK_FILTER_LINEAR;
        envSamplerInfo.minFilter = VK_FILTER_LINEAR;
        envSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        envSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        envSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        envSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        envSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(
                _device, &envSamplerInfo, nullptr,
                &_envSampler) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: environment sampler creation failed");
        }

        // Shared sampler for the separate material images (height, detail
        // normal, displacement). Linear + repeat like the Metal equivalents.
        VkSamplerCreateInfo extraSamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        extraSamplerInfo.magFilter = VK_FILTER_LINEAR;
        extraSamplerInfo.minFilter = VK_FILTER_LINEAR;
        extraSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        extraSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        extraSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        extraSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        extraSamplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(
                _device, &extraSamplerInfo, nullptr,
                &_materialExtraSampler) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: material sampler creation failed");
        }

        // Shadow-map sampler: clamp-to-edge, NEAREST filter, no mips.  A plain
        // (non-comparison) sampler — the shader does the depth compare manually
        // and averages a 3×3 PCF kernel at discrete texel offsets, so no linear
        // filtering is needed (and depth formats like D32_SFLOAT often don't
        // support VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR anyway).
        VkSamplerCreateInfo shadowSamplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        shadowSamplerInfo.magFilter = VK_FILTER_NEAREST;
        shadowSamplerInfo.minFilter = VK_FILTER_NEAREST;
        shadowSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        shadowSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        shadowSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        shadowSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        shadowSamplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(
                _device, &shadowSamplerInfo, nullptr,
                &_shadowSampler) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: shadow sampler creation failed");
        }

        // 1×1 white texture (fallback for unbound texture slots)
        {
            VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imgInfo.imageType = VK_IMAGE_TYPE_2D;
            imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imgInfo.extent = {1, 1, 1};
            imgInfo.mipLevels = 1;
            imgInfo.arrayLayers = 1;
            imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

            VmaAllocationCreateInfo aInfo{};
            aInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            if (vmaCreateImage(
                    _vmaAllocator, &imgInfo, &aInfo, &_whiteImage,
                    &_whiteAllocation, nullptr) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: fallback image creation failed");
            }

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = _whiteImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(
                    _device, &viewInfo, nullptr,
                    &_whiteImageView) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: fallback image view creation failed");
            }

            // Upload single white pixel
            uint32_t whitePixel = 0xFFFFFFFF;
            VkBuffer stagingBuf = VK_NULL_HANDLE;
            VmaAllocation stagingAlloc = VK_NULL_HANDLE;
            VkBufferCreateInfo sInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            sInfo.size = 4;
            sInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo saInfo{};
            saInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            if (vmaCreateBuffer(
                    _vmaAllocator, &sInfo, &saInfo, &stagingBuf,
                    &stagingAlloc, nullptr) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: fallback staging buffer creation failed");
            }
            void* mapped = nullptr;
            if (vmaMapMemory(
                    _vmaAllocator, stagingAlloc, &mapped) != VK_SUCCESS) {
                vmaDestroyBuffer(
                    _vmaAllocator, stagingBuf, stagingAlloc);
                throw std::runtime_error(
                    "VulkanGraphicsDevice: fallback staging buffer mapping failed");
            }
            memcpy(mapped, &whitePixel, 4);
            vmaUnmapMemory(_vmaAllocator, stagingAlloc);

            const VkImage whiteImage = _whiteImage;
            enqueueUpload([whiteImage, stagingBuf](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, whiteImage,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {1, 1, 1};
                vkCmdCopyBufferToImage(cmd, stagingBuf, whiteImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                vulkanTransitionImageLayout(cmd, whiteImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }, [allocator = _vmaAllocator, stagingBuf, stagingAlloc] {
                vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
            });
        }

        // 1×1 white cubemap (fallback for unbound omni shadow slots).  Six
        // layers + cube-compatible so it can back a samplerCube descriptor;
        // every face is white so an unshadowed omni light reads fully lit.
        {
            VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imgInfo.imageType = VK_IMAGE_TYPE_2D;
            imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            imgInfo.extent = {1, 1, 1};
            imgInfo.mipLevels = 1;
            imgInfo.arrayLayers = 6;
            imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            imgInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

            VmaAllocationCreateInfo aInfo{};
            aInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            if (vmaCreateImage(
                    _vmaAllocator, &imgInfo, &aInfo, &_whiteCubeImage,
                    &_whiteCubeAllocation, nullptr) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: fallback cubemap creation failed");
            }

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = _whiteCubeImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
            if (vkCreateImageView(
                    _device, &viewInfo, nullptr,
                    &_whiteCubeImageView) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: fallback cubemap view creation failed");
            }

            uint32_t whitePixels[6] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                       0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
            VkBuffer stagingBuf = VK_NULL_HANDLE;
            VmaAllocation stagingAlloc = VK_NULL_HANDLE;
            VkBufferCreateInfo sInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            sInfo.size = sizeof(whitePixels);
            sInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo saInfo{};
            saInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            if (vmaCreateBuffer(
                    _vmaAllocator, &sInfo, &saInfo, &stagingBuf,
                    &stagingAlloc, nullptr) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: fallback cubemap staging buffer "
                    "creation failed");
            }
            void* mapped = nullptr;
            if (vmaMapMemory(
                    _vmaAllocator, stagingAlloc, &mapped) != VK_SUCCESS) {
                vmaDestroyBuffer(
                    _vmaAllocator, stagingBuf, stagingAlloc);
                throw std::runtime_error(
                    "VulkanGraphicsDevice: fallback cubemap staging buffer "
                    "mapping failed");
            }
            memcpy(mapped, whitePixels, sizeof(whitePixels));
            vmaUnmapMemory(_vmaAllocator, stagingAlloc);

            const VkImage whiteCubeImage = _whiteCubeImage;
            enqueueUpload([whiteCubeImage, stagingBuf](VkCommandBuffer cmd) {
                vulkanTransitionImageLayout(cmd, whiteCubeImage,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);
                // One copy per face (each face is a distinct array layer).
                VkBufferImageCopy regions[6]{};
                for (uint32_t f = 0; f < 6; ++f) {
                    regions[f].bufferOffset = f * sizeof(uint32_t);
                    regions[f].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
                    regions[f].imageExtent = {1, 1, 1};
                }
                vkCmdCopyBufferToImage(cmd, stagingBuf, whiteCubeImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, regions);
                vulkanTransitionImageLayout(cmd, whiteCubeImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6);
            }, [allocator = _vmaAllocator, stagingBuf, stagingAlloc] {
                vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
            });
        }

        // Per-draw / per-pass uniform ring buffer.  Sized for a generous draw
        // count per frame: each region holds material + lighting slots for the
        // whole frame.  Slot sizes are aligned up to the device's dynamic-UBO
        // offset granularity inside the ring allocator.
        {
            constexpr VkDeviceSize kRegionBytes = 8u * 1024u * 1024u;  // 8 MB / frame
            _uniformRing = std::make_unique<VulkanUniformRingBuffer>(
                _vmaAllocator, kMaxFramesInFlight, kRegionBytes, _uboOffsetAlignment);

            // Persistent pool for the two dynamic-UBO descriptor sets.  Never
            // reset — the sets reference the stable ring buffer and only their
            // dynamic offsets change per draw.
            std::array<VkDescriptorPoolSize, 1> sizes{};
            sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2};
            VkDescriptorPoolCreateInfo dpInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpInfo.maxSets = 2;
            dpInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
            dpInfo.pPoolSizes = sizes.data();
            if (vkCreateDescriptorPool(
                    _device, &dpInfo, nullptr, &_persistentDescriptorPool) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: persistent descriptor pool creation failed");
            }

            auto allocSet = [&](VkDescriptorSetLayout layout, VkDeviceSize range) {
                VkDescriptorSet set = VK_NULL_HANDLE;
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = _persistentDescriptorPool;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &layout;
                if (vkAllocateDescriptorSets(_device, &ai, &set) != VK_SUCCESS) {
                    throw std::runtime_error(
                        "VulkanGraphicsDevice: persistent descriptor allocation failed");
                }

                VkDescriptorBufferInfo bi{};
                bi.buffer = _uniformRing->buffer();
                bi.offset = 0;            // base; per-draw dynamic offset supplies the slot
                bi.range = range;         // size of one struct, not the whole buffer
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = set;
                w.dstBinding = 0;
                w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                w.descriptorCount = 1;
                w.pBufferInfo = &bi;
                vkUpdateDescriptorSets(_device, 1, &w, 0, nullptr);
                return set;
            };

            _materialDescriptorSet = allocSet(_renderPipeline->materialSetLayout(),
                sizeof(MaterialUniforms));
            _lightingDescriptorSet = allocSet(_renderPipeline->lightingSetLayout(),
                sizeof(VulkanLightingUBO));
        }

        spdlog::info("VulkanGraphicsDevice initialized ({}x{})", _width, _height);
    }

    void VulkanGraphicsDevice::destroySamplers() noexcept
    {
        if (_device == VK_NULL_HANDLE) return;

        // Keep this list exhaustive: it is the ONLY place the constructor's
        // samplers are released, so a new sampler that is not added here leaks
        // on every device teardown.
        for (VkSampler* sampler : {&_shadowSampler, &_envSampler,
                 &_materialExtraSampler, &_defaultSampler}) {
            if (*sampler != VK_NULL_HANDLE) {
                vkDestroySampler(_device, *sampler, nullptr);
                *sampler = VK_NULL_HANDLE;
            }
        }
    }

    VulkanGraphicsDevice::~VulkanGraphicsDevice()
    {
        flushUploads();
        if (_device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(_device);
        collectUploads(true);

        // GraphicsDevice owns shaders, buffers, textures, and render targets
        // whose destructors need a live VkDevice/VMA allocator. Release them
        // before tearing down native state, then drain their deferred destroys.
        releaseGpuReferences();
        _sceneColorGrabTexture.reset();
        _sceneDepthGrabTexture.reset();
        flushDeferredDestroys(true);
        collectRetiredSwapchains(true);

        destroyPostResources();
        destroyComputeResources();

        for (auto& [key, pipeline] : _vsmBlurPipelines) {
            vkDestroyPipeline(_device, pipeline, nullptr);
        }
        if (_vsmBlurPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(_device, _vsmBlurPipelineLayout, nullptr);
        if (_vsmBlurSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(_device, _vsmBlurSetLayout, nullptr);
        if (_vsmBlurVertModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(_device, _vsmBlurVertModule, nullptr);
        if (_vsmBlurFragModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(_device, _vsmBlurFragModule, nullptr);

        _renderPipeline.reset();

        if (_persistentDescriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(_device, _persistentDescriptorPool, nullptr);
        _uniformRing.reset();

        destroySamplers();
        if (_whiteImageView != VK_NULL_HANDLE)
            vkDestroyImageView(_device, _whiteImageView, nullptr);
        if (_whiteImage != VK_NULL_HANDLE)
            vmaDestroyImage(_vmaAllocator, _whiteImage, _whiteAllocation);
        if (_whiteCubeImageView != VK_NULL_HANDLE)
            vkDestroyImageView(_device, _whiteCubeImageView, nullptr);
        if (_whiteCubeImage != VK_NULL_HANDLE)
            vmaDestroyImage(_vmaAllocator, _whiteCubeImage, _whiteCubeAllocation);

        destroyPerFrameResources();

        if (_uploadCommandPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(_device, _uploadCommandPool, nullptr);

        destroyDepthResources();
        cleanupSwapchain();

        if (_vmaAllocator != VK_NULL_HANDLE)
            vmaDestroyAllocator(_vmaAllocator);
        if (_device != VK_NULL_HANDLE)
            vkDestroyDevice(_device, nullptr);
        if (_surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(_instance, _surface, nullptr);
        if (_debugMessenger != VK_NULL_HANDLE) {
            auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(_instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (fn) fn(_instance, _debugMessenger, nullptr);
        }
        if (_instance != VK_NULL_HANDLE)
            vkDestroyInstance(_instance, nullptr);

        spdlog::info("VulkanGraphicsDevice destroyed");
    }

    void VulkanGraphicsDevice::cleanupPartialInitialization() noexcept
    {
        if (_device != VK_NULL_HANDLE) {
            // No constructor stage intentionally submits GPU work, but waiting
            // here also makes this cleanup safe if initialization grows later.
            (void)vkDeviceWaitIdle(_device);
        }

        // Constructor uploads have not been submitted yet. Run their retirement
        // callbacks so staging allocations do not survive until VMA teardown.
        std::vector<PendingUpload> pendingUploads;
        {
            std::lock_guard lock(_uploadMutex);
            pendingUploads.swap(_pendingUploads);
        }
        for (auto& upload : pendingUploads) {
            if (upload.retire) {
                try {
                    upload.retire();
                } catch (...) {
                    // Cleanup must never replace the initialization exception.
                }
            }
        }

        // Objects with destructors that call VkDevice/VMA must die first.
        _renderPipeline.reset();

        if (_device != VK_NULL_HANDLE) {
            if (_persistentDescriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(
                    _device, _persistentDescriptorPool, nullptr);
                _persistentDescriptorPool = VK_NULL_HANDLE;
            }
            destroySamplers();
            if (_whiteImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(_device, _whiteImageView, nullptr);
                _whiteImageView = VK_NULL_HANDLE;
            }
            if (_whiteCubeImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(
                    _device, _whiteCubeImageView, nullptr);
                _whiteCubeImageView = VK_NULL_HANDLE;
            }
        }
        _uniformRing.reset();
        if (_vmaAllocator != VK_NULL_HANDLE) {
            if (_whiteImage != VK_NULL_HANDLE) {
                vmaDestroyImage(
                    _vmaAllocator, _whiteImage, _whiteAllocation);
                _whiteImage = VK_NULL_HANDLE;
                _whiteAllocation = VK_NULL_HANDLE;
            }
            if (_whiteCubeImage != VK_NULL_HANDLE) {
                vmaDestroyImage(
                    _vmaAllocator, _whiteCubeImage,
                    _whiteCubeAllocation);
                _whiteCubeImage = VK_NULL_HANDLE;
                _whiteCubeAllocation = VK_NULL_HANDLE;
            }
        }

        if (_device != VK_NULL_HANDLE) {
            destroyPerFrameResources();
            if (_uploadCommandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(
                    _device, _uploadCommandPool, nullptr);
                _uploadCommandPool = VK_NULL_HANDLE;
            }
        }
        if (_vmaAllocator != VK_NULL_HANDLE) {
            destroyDepthResources();
        }
        if (_device != VK_NULL_HANDLE) {
            cleanupSwapchain();
        }

        if (_vmaAllocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(_vmaAllocator);
            _vmaAllocator = VK_NULL_HANDLE;
        }
        if (_device != VK_NULL_HANDLE) {
            vkDestroyDevice(_device, nullptr);
            _device = VK_NULL_HANDLE;
        }
        if (_surface != VK_NULL_HANDLE &&
            _instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(_instance, _surface, nullptr);
            _surface = VK_NULL_HANDLE;
        }
        if (_debugMessenger != VK_NULL_HANDLE &&
            _instance != VK_NULL_HANDLE) {
            const auto destroyMessenger =
                reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(
                        _instance,
                        "vkDestroyDebugUtilsMessengerEXT"));
            if (destroyMessenger) {
                destroyMessenger(
                    _instance, _debugMessenger, nullptr);
            }
            _debugMessenger = VK_NULL_HANDLE;
        }
        if (_instance != VK_NULL_HANDLE) {
            vkDestroyInstance(_instance, nullptr);
            _instance = VK_NULL_HANDLE;
        }
    }

    void VulkanGraphicsDevice::initInstance(SDL_Window* window)
    {
        vkb::InstanceBuilder builder;
        builder.set_app_name("VisuTwin Canvas")
               .set_engine_name("VisuTwin")
               .require_api_version(1, 3, 0);

        if (_validationEnabled) {
            builder.enable_validation_layers()
                   .set_debug_callback(vulkanValidationCallback)
                   .set_debug_callback_user_data_pointer(_validationErrorCount.get())
                   .set_debug_messenger_severity(
                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                   .set_debug_messenger_type(
                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT);
        }

        auto result = builder.build();
        if (!result) {
            spdlog::error("Failed to create Vulkan instance: {}", result.error().message());
            return;
        }
        auto vkbInstance = result.value();
        _instance = vkbInstance.instance;
        _debugMessenger = vkbInstance.debug_messenger;
        if (_validationEnabled) {
            spdlog::info("Vulkan validation enabled");
        }

        if (!SDL_Vulkan_CreateSurface(window, _instance, nullptr, &_surface)) {
            spdlog::error("Failed to create Vulkan surface");
        }
    }

    void VulkanGraphicsDevice::initDevice()
    {
        // Require Vulkan 1.3 — dynamicRendering and synchronization2 are
        // promoted-to-core there, so we can use the core entry points
        // directly (vkCmdBeginRendering etc.).  MoltenVK 1.3+ supports this
        // on Apple Silicon.
        //
        // DeviceBuilder retains these pNext pointers only through the
        // synchronous build() call below, so local storage is sufficient and
        // keeps concurrent device initialization independent. vkb's
        // set_required_features_13() vets support but does not propagate the
        // struct into VkDeviceCreateInfo::pNext; add_pNext enables the
        // features on the created device.
        VkPhysicalDeviceVulkan13Features features13{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;

        // vkb::Instance is an aggregate; construct it field-by-field rather
        // than via a 2-arg ctor (which it doesn't have).
        vkb::Instance vkbInst{};
        vkbInst.instance = _instance;
        vkbInst.debug_messenger = _debugMessenger;
        vkb::PhysicalDeviceSelector selector{vkbInst};
        selector.set_surface(_surface)
                .set_minimum_version(1, 3);

        auto physResult = selector.select();
        if (!physResult) {
            spdlog::error("Failed to select Vulkan physical device: {}", physResult.error().message());
            return;
        }
        auto vkbPhysical = physResult.value();
        _physicalDevice = vkbPhysical.physical_device;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(_physicalDevice, &props);
        spdlog::info("Vulkan device: {}, apiVersion={}.{}.{}", props.deviceName,
            VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion));

        // Dynamic UBO offsets must be a multiple of this (256 on MoltenVK).
        _uboOffsetAlignment = std::max(
            props.limits.minUniformBufferOffsetAlignment,
            props.limits.minStorageBufferOffsetAlignment);
        if (_uboOffsetAlignment == 0) _uboOffsetAlignment = 256;

        // Enable anisotropic filtering when the hardware has it (MoltenVK on
        // Apple GPUs does). Requested via a Vulkan-1.0 features struct chained
        // alongside features13 for the duration of build().
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(_physicalDevice, &supported);
        _samplerAnisotropyEnabled = supported.samplerAnisotropy == VK_TRUE;
        _maxSamplerAnisotropy = _samplerAnisotropyEnabled
            ? std::min(16.0f, props.limits.maxSamplerAnisotropy) : 1.0f;

        // Dual-source blending (the BLENDMODE_SRC1_* factors) is an optional
        // Vulkan feature and must be enabled at device creation before
        // VK_BLEND_FACTOR_SRC1_* may appear in any pipeline. Opt in whenever the
        // hardware offers it; supportsDualSourceBlending() reports the result so
        // callers can check before building such a blend state.
        _dualSrcBlendEnabled = supported.dualSrcBlend == VK_TRUE;
        _maxDualSrcDrawBuffers = _dualSrcBlendEnabled
            ? props.limits.maxFragmentDualSrcAttachments : 0;
        if (_dualSrcBlendEnabled) {
            spdlog::info("Vulkan dual-source blending: enabled "
                "(maxFragmentDualSrcAttachments={})", _maxDualSrcDrawBuffers);
        } else {
            spdlog::info("Vulkan dual-source blending: unsupported by this device");
        }

        VkPhysicalDeviceFeatures2 features2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.features.samplerAnisotropy = _samplerAnisotropyEnabled ? VK_TRUE : VK_FALSE;
        features2.features.dualSrcBlend = _dualSrcBlendEnabled ? VK_TRUE : VK_FALSE;

        vkb::DeviceBuilder deviceBuilder{vkbPhysical};
        deviceBuilder.add_pNext(&features13);
        deviceBuilder.add_pNext(&features2);
        auto devResult = deviceBuilder.build();
        if (!devResult) {
            spdlog::error("Failed to create Vulkan device: {}", devResult.error().message());
            return;
        }
        auto vkbDevice = devResult.value();
        _device = vkbDevice.device;

        const auto graphicsQueue =
            vkbDevice.get_queue(vkb::QueueType::graphics);
        const auto graphicsQueueFamily =
            vkbDevice.get_queue_index(vkb::QueueType::graphics);
        const auto presentQueue =
            vkbDevice.get_queue(vkb::QueueType::present);
        const auto presentQueueFamily =
            vkbDevice.get_queue_index(vkb::QueueType::present);
        if (!graphicsQueue || !graphicsQueueFamily ||
            !presentQueue || !presentQueueFamily) {
            spdlog::error(
                "Failed to retrieve required Vulkan graphics/presentation queues");
            return;
        }

        _graphicsQueue = graphicsQueue.value();
        _graphicsQueueFamily = graphicsQueueFamily.value();
        _presentQueue = presentQueue.value();
        _presentQueueFamily = presentQueueFamily.value();
        spdlog::info(
            "Vulkan queue families: graphics={}, present={}{}",
            _graphicsQueueFamily, _presentQueueFamily,
            _graphicsQueueFamily == _presentQueueFamily
                ? " (shared)" : " (dedicated presentation queue)");
    }

    std::shared_ptr<Shader> VulkanGraphicsDevice::createShader(
        const ShaderDefinition& definition, const std::string& sourceCode)
    {
        const bool isShadowName = definition.name.rfind("program-shadow", 0) == 0;

        if (!definition.cshader.empty()) {
            if (sourceCode.empty() || !looksLikeGlsl(sourceCode)) {
                spdlog::error(
                    "VulkanGraphicsDevice::createShader('{}'): compute shaders require GLSL source",
                    definition.name);
                return nullptr;
            }
            if (!vulkanShaderCompilerAvailable()) {
                spdlog::error(
                    "VulkanGraphicsDevice::createShader('{}'): runtime compute GLSL requires shaderc",
                    definition.name);
                return nullptr;
            }
            auto computeSpv = vulkanCompileGlsl(sourceCode,
                VulkanShaderStage::Compute, definition.name + ".comp");
            if (computeSpv.empty()) {
                return nullptr;
            }
            return std::make_shared<VulkanShader>(this, definition,
                nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
                nullptr, 0, nullptr, 0, false,
                computeSpv.data(), computeSpv.size());
        }

        // Custom GLSL source: compile at runtime via shaderc. The single
        // source is compiled twice with VT_VERTEX_SHADER / VT_FRAGMENT_SHADER
        // defines so authors can guard the stages with #ifdef. A failed custom
        // shader is an error; it never mutates into an unrelated forward shader.
        if (!sourceCode.empty() && looksLikeGlsl(sourceCode) && vulkanShaderCompilerAvailable()) {
            auto vertSpv = vulkanCompileGlsl(sourceCode, VulkanShaderStage::Vertex,
                definition.name + ".vert", {{"VT_VERTEX_SHADER", "1"}});
            auto fragSpv = vulkanCompileGlsl(sourceCode, VulkanShaderStage::Fragment,
                definition.name + ".frag", {{"VT_FRAGMENT_SHADER", "1"}});
            if (!vertSpv.empty() && !fragSpv.empty()) {
                spdlog::info("VulkanGraphicsDevice::createShader('{}'): compiled custom GLSL at runtime",
                    definition.name);
                return std::make_shared<VulkanShader>(this, definition,
                    vertSpv.data(), vertSpv.size(),
                    fragSpv.data(), fragSpv.size());
            }
            spdlog::error(
                "VulkanGraphicsDevice::createShader('{}'): custom GLSL failed",
                definition.name);
            return nullptr;
        } else if (!sourceCode.empty() && looksLikeGlsl(sourceCode)) {
            spdlog::error(
                "VulkanGraphicsDevice::createShader('{}'): runtime custom GLSL "
                "requires shaderc", definition.name);
            return nullptr;
        } else if (!sourceCode.empty() && !looksLikeGlsl(sourceCode)) {
            // ProgramLibrary composes MSL for Metal, but the definition also
            // carries the shared feature mask consumed by the build-time
            // Vulkan module family. Arbitrary ShaderMaterial MSL is not a
            // Vulkan program and must fail explicitly.
            if (definition.name.rfind("program-", 0) != 0) {
                spdlog::error(
                    "VulkanGraphicsDevice::createShader('{}'): custom MSL "
                    "cannot be used by Vulkan; provide GLSL", definition.name);
                return nullptr;
            }
        }

        const uint32_t* fragment = isShadowName
            ? vulkan_generated::kShadowVsmFrag
            : vulkan_generated::kForwardFrag;
        const size_t fragmentWords = isShadowName
            ? vulkan_generated::kShadowVsmFragWordCount
            : vulkan_generated::kForwardFragWordCount;
        if (definition.name == "particles") {
            return std::make_shared<VulkanShader>(this, definition,
                vulkan_generated::kParticleVert,
                vulkan_generated::kParticleVertWordCount,
                vulkan_generated::kParticleFrag,
                vulkan_generated::kParticleFragWordCount);
        }
        if (definition.name == "gsplat") {
            return std::make_shared<VulkanShader>(this, definition,
                vulkan_generated::kGSplatVert,
                vulkan_generated::kGSplatVertWordCount,
                vulkan_generated::kGSplatFrag,
                vulkan_generated::kGSplatFragWordCount);
        }
        return std::make_shared<VulkanShader>(this, definition,
            vulkan_generated::kForwardVert,
            vulkan_generated::kForwardVertWordCount,
            fragment, fragmentWords,
            vulkan_generated::kForwardInstancedVert,
            vulkan_generated::kForwardInstancedVertWordCount,
            vulkan_generated::kForwardSkyVert,
            vulkan_generated::kForwardSkyVertWordCount,
            vulkan_generated::kForwardColorVert,
            vulkan_generated::kForwardColorVertWordCount,
            vulkan_generated::kForwardPointVert,
            vulkan_generated::kForwardPointVertWordCount,
            vulkan_generated::kForwardDynamicBatchVert,
            vulkan_generated::kForwardDynamicBatchVertWordCount,
            vulkan_generated::kForwardSkinnedVert,
            vulkan_generated::kForwardSkinnedVertWordCount,
            vulkan_generated::kForwardMorphedVert,
            vulkan_generated::kForwardMorphedVertWordCount,
            vulkan_generated::kForwardSkinnedMorphedVert,
            vulkan_generated::kForwardSkinnedMorphedVertWordCount,
            !isShadowName);
    }

    std::unique_ptr<gpu::HardwareTexture> VulkanGraphicsDevice::createGPUTexture(Texture* texture)
    {
        return std::make_unique<gpu::VulkanTexture>(texture);
    }

    std::shared_ptr<VertexBuffer> VulkanGraphicsDevice::createVertexBuffer(
        const std::shared_ptr<VertexFormat>& format, int numVertices,
        const VertexBufferOptions& options)
    {
        return std::make_shared<VulkanVertexBuffer>(this, format, numVertices, options);
    }

    std::shared_ptr<VertexBuffer> VulkanGraphicsDevice::createVertexBufferFromNativeBuffer(
        const std::shared_ptr<VertexFormat>& format, int numVertices, void* nativeBuffer)
    {
        const VkBuffer buffer = reinterpret_cast<VkBuffer>(nativeBuffer);
        if (buffer == VK_NULL_HANDLE) return nullptr;
        return std::make_shared<VulkanVertexBuffer>(this, format, numVertices, buffer);
    }

    std::unique_ptr<InstanceCuller> VulkanGraphicsDevice::createInstanceCuller()
    {
        return std::make_unique<VulkanInstanceCullPass>(this);
    }

    std::shared_ptr<IndexBuffer> VulkanGraphicsDevice::createIndexBuffer(
        IndexFormat format, int numIndices, const std::vector<uint8_t>& data)
    {
        auto ib = std::make_shared<VulkanIndexBuffer>(this, format, numIndices);
        if (!data.empty()) ib->setData(data);
        return ib;
    }

    std::shared_ptr<RenderTarget> VulkanGraphicsDevice::createRenderTarget(
        const RenderTargetOptions& options)
    {
        // Caller may pass colorBuffer/depthBuffer textures that have not yet
        // had their device assigned; ensure we backfill it before
        // RenderTarget's constructor runs (it asserts on a non-null device).
        RenderTargetOptions opts = options;
        if (!opts.graphicsDevice) {
            opts.graphicsDevice = this;
        }
        return std::make_shared<VulkanRenderTarget>(opts);
    }
}

#endif // VISUTWIN_HAS_VULKAN
