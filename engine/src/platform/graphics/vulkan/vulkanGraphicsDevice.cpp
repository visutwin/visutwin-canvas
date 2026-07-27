// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan implementation of the graphics device.
//

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
    size_t VulkanGraphicsDevice::ImageDescriptorKeyHash::operator()(
        const ImageDescriptorKey& key) const
    {
        size_t hash = std::hash<VkDescriptorSetLayout>{}(key.layout);
        const auto combine = [&hash](const size_t value) {
            hash ^= value + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
        };
        combine(std::hash<uint32_t>{}(key.count));
        for (uint32_t i = 0; i < key.count; ++i) {
            combine(std::hash<VkSampler>{}(key.samplers[i]));
            combine(std::hash<VkImageView>{}(key.views[i]));
        }
        return hash;
    }

    namespace
    {
        // Detects GLSL custom shader source (the engine's composed shader
        // variants are MSL, which no Vulkan compiler consumes).
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
}


namespace visutwin::canvas
{
    std::atomic_int
        VulkanGraphicsDevice::_initializationFailureCheckpoint{0};

    // ─────────────────────────────────────────────────────────────────────
    // Construction / Destruction
    // ─────────────────────────────────────────────────────────────────────

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
        if (!SDL_GetWindowSize(_window, &w, &h)) {
            throw std::runtime_error(
                std::string(
                    "VulkanGraphicsDevice: failed to query window size: ") +
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

        if (_shadowSampler != VK_NULL_HANDLE)
            vkDestroySampler(_device, _shadowSampler, nullptr);
        if (_envSampler != VK_NULL_HANDLE)
            vkDestroySampler(_device, _envSampler, nullptr);
        if (_defaultSampler != VK_NULL_HANDLE)
            vkDestroySampler(_device, _defaultSampler, nullptr);
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
            if (_shadowSampler != VK_NULL_HANDLE) {
                vkDestroySampler(_device, _shadowSampler, nullptr);
                _shadowSampler = VK_NULL_HANDLE;
            }
            if (_envSampler != VK_NULL_HANDLE) {
                vkDestroySampler(_device, _envSampler, nullptr);
                _envSampler = VK_NULL_HANDLE;
            }
            if (_defaultSampler != VK_NULL_HANDLE) {
                vkDestroySampler(_device, _defaultSampler, nullptr);
                _defaultSampler = VK_NULL_HANDLE;
            }
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

    // ─────────────────────────────────────────────────────────────────────
    // Initialization helpers
    // ─────────────────────────────────────────────────────────────────────

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
        // The feature struct is `static` because we hand its address to
        // DeviceBuilder::add_pNext() which keeps the pointer alive until the
        // build() call — vkb's set_required_features_13() vets support but
        // doesn't propagate the struct into VkDeviceCreateInfo::pNext, which
        // leaves dynamicRendering disabled at the device level (validation:
        // VUID-vkCmdBeginRendering-dynamicRendering-06446).  add_pNext pins
        // the feature on so the device creation honours it.
        static VkPhysicalDeviceVulkan13Features features13{
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
        // like features13; static for the same pointer-lifetime reason.
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(_physicalDevice, &supported);
        _samplerAnisotropyEnabled = supported.samplerAnisotropy == VK_TRUE;
        _maxSamplerAnisotropy = _samplerAnisotropyEnabled
            ? std::min(16.0f, props.limits.maxSamplerAnisotropy) : 1.0f;
        static VkPhysicalDeviceFeatures2 features2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.features.samplerAnisotropy = _samplerAnisotropyEnabled ? VK_TRUE : VK_FALSE;

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

    bool VulkanGraphicsDevice::initSwapchain(
        const int width, const int height,
        const VkSwapchainKHR oldSwapchain)
    {
        // Use a linear (UNORM) swapchain — the shaders apply manual
        // pow(1/2.2) for display gamma encoding, matching the Metal path
        // which renders into a non-sRGB BGRA8Unorm drawable.  Choosing
        // VK_FORMAT_B8G8R8A8_SRGB instead would make the hardware apply a
        // second sRGB encode on store, doubling the gamma and washing out
        // the rendered scene.
        // Passing both family indices makes vk-bootstrap use concurrent image
        // sharing when graphics and presentation are on different families,
        // avoiding explicit queue-family ownership transfers for each frame.
        vkb::SwapchainBuilder swapBuilder{
            _physicalDevice, _device, _surface,
            _graphicsQueueFamily, _presentQueueFamily};
        swapBuilder.set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
                   .set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                   .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                   .add_image_usage_flags(
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (oldSwapchain != VK_NULL_HANDLE) {
            swapBuilder.set_old_swapchain(oldSwapchain);
        }

        auto result = swapBuilder.build();
        if (!result) {
            spdlog::error("Failed to create Vulkan swapchain: {}", result.error().message());
            return false;
        }
        auto vkbSwap = result.value();
        auto imagesResult = vkbSwap.get_images();
        auto viewsResult = vkbSwap.get_image_views();
        if (!imagesResult || !viewsResult) {
            if (viewsResult) {
                vkbSwap.destroy_image_views(viewsResult.value());
            }
            vkb::destroy_swapchain(vkbSwap);
            // A successful vkCreateSwapchainKHR retires oldSwapchain even if
            // subsequent setup fails, so the caller must not restore it.
            _swapchain = VK_NULL_HANDLE;
            spdlog::error(
                "Failed to retrieve Vulkan swapchain images or image views");
            return false;
        }

        _swapchain = vkbSwap.swapchain;
        _swapchainFormat = vkbSwap.image_format;
        _swapchainExtent = vkbSwap.extent;
        _swapchainImages = std::move(imagesResult.value());
        _swapchainImageViews = std::move(viewsResult.value());

        // Adopt the actual swapchain extent as the device size.  The requested
        // width/height can be stale or zero — SDL_GetWindowSize may report 0×0
        // before the window is first shown — but the surface clamps the
        // swapchain to its real size.  size() (and therefore the renderer's
        // viewport/scissor) must reflect that, or every draw collapses to a
        // 1×1 viewport.
        _width = static_cast<int>(_swapchainExtent.width);
        _height = static_cast<int>(_swapchainExtent.height);
        return true;
    }

    void VulkanGraphicsDevice::cleanupSwapchain()
    {
        for (auto view : _swapchainImageViews)
            vkDestroyImageView(_device, view, nullptr);
        _swapchainImageViews.clear();
        _swapchainImages.clear();
        if (_swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(_device, _swapchain, nullptr);
            _swapchain = VK_NULL_HANDLE;
        }
    }

    void VulkanGraphicsDevice::createDepthResources()
    {
        _depthFormat = vulkanSupportedDepthFormat(_physicalDevice);
        if (_depthFormat == VK_FORMAT_UNDEFINED) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: no supported swapchain depth format");
        }

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = _depthFormat;
        imageInfo.extent = {_swapchainExtent.width, _swapchainExtent.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(_vmaAllocator, &imageInfo, &allocInfo,
                &_depthImage, &_depthAllocation, nullptr) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: swapchain depth image allocation failed");
        }

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = _depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = _depthFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(_device, &viewInfo, nullptr, &_depthImageView) !=
            VK_SUCCESS) {
            vmaDestroyImage(_vmaAllocator, _depthImage, _depthAllocation);
            _depthImage = VK_NULL_HANDLE;
            _depthAllocation = VK_NULL_HANDLE;
            throw std::runtime_error(
                "VulkanGraphicsDevice: swapchain depth image view creation failed");
        }
    }

    void VulkanGraphicsDevice::destroyDepthResources()
    {
        if (_depthImageView != VK_NULL_HANDLE)
            vkDestroyImageView(_device, _depthImageView, nullptr);
        if (_depthImage != VK_NULL_HANDLE)
            vmaDestroyImage(_vmaAllocator, _depthImage, _depthAllocation);
        _depthImageView = VK_NULL_HANDLE;
        _depthImage = VK_NULL_HANDLE;
        _depthImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        _depthAllocation = VK_NULL_HANDLE;
    }

    void VulkanGraphicsDevice::createPerFrameResources()
    {
        for (auto& frame : _frames) {
            VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = _graphicsQueueFamily;
            if (vkCreateCommandPool(
                    _device, &poolInfo, nullptr,
                    &frame.commandPool) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: frame command pool creation failed");
            }

            VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(
                    _device, &allocInfo,
                    &frame.commandBuffer) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: frame command buffer allocation failed");
            }

            VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            if (vkCreateSemaphore(
                    _device, &semInfo, nullptr,
                    &frame.imageAvailable) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: acquire semaphore creation failed");
            }

            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(
                    _device, &fenceInfo, nullptr,
                    &frame.inFlightFence) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: frame fence creation failed");
            }

            const VkDescriptorPool descriptorPool =
                createFrameDescriptorPool(kInitialDescriptorSets);
            if (descriptorPool == VK_NULL_HANDLE) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: per-frame descriptor pool creation failed");
            }
            frame.descriptorPools.push_back(
                {descriptorPool, kInitialDescriptorSets});
        }

        if (!createSwapchainSemaphores()) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: presentation semaphore creation failed");
        }
    }

    VkDescriptorPool VulkanGraphicsDevice::createFrameDescriptorPool(
        const uint32_t maxSets)
    {
        // Every cached image set has at most seven combined samplers. Post
        // passes additionally consume one ordinary UBO descriptor per set.
        std::array<VkDescriptorPoolSize, 3> poolSizes{};
        poolSizes[0] = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            maxSets * kMaxCachedImageBindings
        };
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets};
        poolSizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxSets * 2};

        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.maxSets = maxSets;
        info.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        info.pPoolSizes = poolSizes.data();

        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(_device, &info, nullptr, &pool) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        return pool;
    }

    VkDescriptorSet VulkanGraphicsDevice::allocateFrameDescriptorSet(
        const VkDescriptorSetLayout layout)
    {
        auto& frame = _frames[_frameIndex];
        for (;;) {
            if (frame.activeDescriptorPool >= frame.descriptorPools.size()) {
                constexpr size_t kMaxGrowthSteps = 4;
                const size_t growthStep =
                    std::min(frame.descriptorPools.size(), kMaxGrowthSteps);
                const uint32_t maxSets =
                    kInitialDescriptorSets << static_cast<uint32_t>(growthStep);
                const VkDescriptorPool pool = createFrameDescriptorPool(maxSets);
                if (pool == VK_NULL_HANDLE) {
                    if (!_descriptorAllocationErrorWarned) {
                        _descriptorAllocationErrorWarned = true;
                        spdlog::error(
                            "VulkanGraphicsDevice: descriptor pool growth failed");
                    }
                    return VK_NULL_HANDLE;
                }
                frame.descriptorPools.push_back({pool, maxSets});
            }

            const VkDescriptorPool pool =
                frame.descriptorPools[frame.activeDescriptorPool].handle;
            if (pool == VK_NULL_HANDLE) {
                ++frame.activeDescriptorPool;
                continue;
            }
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo info{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            info.descriptorPool = pool;
            info.descriptorSetCount = 1;
            info.pSetLayouts = &layout;
            const VkResult result =
                vkAllocateDescriptorSets(_device, &info, &set);
            if (result == VK_SUCCESS) {
                return set;
            }
            if (result == VK_ERROR_OUT_OF_POOL_MEMORY ||
                result == VK_ERROR_FRAGMENTED_POOL) {
                ++frame.activeDescriptorPool;
                continue;
            }
            if (!_descriptorAllocationErrorWarned) {
                _descriptorAllocationErrorWarned = true;
                spdlog::error(
                    "VulkanGraphicsDevice: descriptor allocation failed ({})",
                    static_cast<int>(result));
            }
            return VK_NULL_HANDLE;
        }
    }

    VkDescriptorSet VulkanGraphicsDevice::getOrCreateImageDescriptorSet(
        const VkDescriptorSetLayout layout,
        const std::span<const VkDescriptorImageInfo> imageInfos)
    {
        if (imageInfos.empty() ||
            imageInfos.size() > kMaxCachedImageBindings) {
            return VK_NULL_HANDLE;
        }

        ImageDescriptorKey key{};
        key.layout = layout;
        key.count = static_cast<uint32_t>(imageInfos.size());
        for (uint32_t i = 0; i < key.count; ++i) {
            key.samplers[i] = imageInfos[i].sampler;
            key.views[i] = imageInfos[i].imageView;
        }

        auto& cache = _frames[_frameIndex].imageDescriptorCache;
        if (const auto found = cache.find(key); found != cache.end()) {
            return found->second;
        }

        const VkDescriptorSet set = allocateFrameDescriptorSet(layout);
        if (set == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }

        std::array<VkWriteDescriptorSet, kMaxCachedImageBindings> writes{};
        constexpr std::array<uint32_t, 6> materialBindings =
            {0, 1, 3, 4, 5, 19};
        const bool materialSet =
            layout == _renderPipeline->textureSetLayout() &&
            key.count == materialBindings.size();
        for (uint32_t i = 0; i < key.count; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = materialSet ? materialBindings[i] : i;
            writes[i].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo = &imageInfos[i];
        }
        vkUpdateDescriptorSets(_device, key.count, writes.data(), 0, nullptr);
        cache.emplace(key, set);
        return set;
    }

    std::optional<uint32_t> VulkanGraphicsDevice::allocateUniform(
        const void* data, const VkDeviceSize size)
    {
        const auto offset = _uniformRing->allocate(data, size);
        if (!offset && !_uniformOverflowWarned) {
            _uniformOverflowWarned = true;
            spdlog::error(
                "VulkanGraphicsDevice: uniform ring allocation failed "
                "(requested {}, used {} of {} bytes); skipping affected draws",
                size, _uniformRing->usedBytes(),
                _uniformRing->capacityPerFrame());
        }
        return offset;
    }

    bool VulkanGraphicsDevice::createSwapchainSemaphores()
    {
        // One renderFinished semaphore per swapchain image — recreated
        // alongside the swapchain because the image count can change on
        // resize.
        _renderFinishedSemaphores.resize(_swapchainImages.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : _renderFinishedSemaphores) {
            if (vkCreateSemaphore(
                    _device, &semInfo, nullptr, &s) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    void VulkanGraphicsDevice::destroySwapchainSemaphores()
    {
        for (auto& s : _renderFinishedSemaphores) {
            if (s != VK_NULL_HANDLE) vkDestroySemaphore(_device, s, nullptr);
        }
        _renderFinishedSemaphores.clear();
    }

    void VulkanGraphicsDevice::destroyPerFrameResources()
    {
        destroySwapchainSemaphores();

        for (auto& frame : _frames) {
            for (const auto& pool : frame.descriptorPools) {
                if (pool.handle != VK_NULL_HANDLE) {
                    vkDestroyDescriptorPool(
                        _device, pool.handle, nullptr);
                }
            }
            if (frame.inFlightFence != VK_NULL_HANDLE)
                vkDestroyFence(_device, frame.inFlightFence, nullptr);
            if (frame.imageAvailable != VK_NULL_HANDLE)
                vkDestroySemaphore(_device, frame.imageAvailable, nullptr);
            if (frame.commandPool != VK_NULL_HANDLE)
                vkDestroyCommandPool(_device, frame.commandPool, nullptr);
            frame = {};
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Frame lifecycle
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::onFrameStart()
    {
        _frameActive = false;
        if (_renderingDisabled) {
            return;
        }
        if (_swapchainRecreationPending) {
            if (!recreateSwapchain() || _swapchainRecreationPending) {
                return;
            }
        }
        auto& frame = _frames[_frameIndex];

        const VkResult waitResult =
            vkWaitForFences(
                _device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: frame fence wait failed ({}); "
                "rendering disabled",
                static_cast<int>(waitResult));
            return;
        }
        collectUploads(false);
        flushUploads();

        // The fence wait proves frame (_frameNumber - kMaxFramesInFlight) has
        // completed on the GPU — release resources queued up to that frame.
        flushDeferredDestroys(false);
        collectRetiredSwapchains(false);
        if (_renderingDisabled) {
            return;
        }

        VkResult result = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &_swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Recreate directly — setResolution(_width, _height) would
            // early-return on the unchanged size and never rebuild the
            // swapchain, wedging every subsequent frame.
            if (!recreateSwapchain()) {
                return;
            }
            if (_swapchainRecreationPending) {
                return;
            }
            result = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
                frame.imageAvailable, VK_NULL_HANDLE, &_swapchainImageIndex);
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            // Skip this frame: the fence was deliberately NOT reset, so the
            // next onFrameStart's wait completes immediately. startRenderPass
            // and onFrameEnd check _frameActive and no-op.
            spdlog::warn("VulkanGraphicsDevice: swapchain acquire failed ({}) — skipping frame",
                static_cast<int>(result));
            return;
        }

        _frameActive = true;

        // A fresh swapchain image is always in an undefined layout — vkAcquire
        // doesn't promise any particular contents.  Track this so onFrameEnd
        // can pick the right source layout for the PRESENT transition; and so
        // startRenderPass can use UNDEFINED as the discard source when first
        // attaching the image (which is fine because it gets LOAD_OP_CLEAR /
        // LOAD_OP_DONT_CARE on the colour attachment).
        _swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        const VkResult resetCommandResult =
            vkResetCommandBuffer(frame.commandBuffer, 0);
        if (resetCommandResult != VK_SUCCESS) {
            _frameActive = false;
            recoverFailedFrameSubmission(resetCommandResult);
            return;
        }

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        const VkResult beginResult =
            vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        if (beginResult != VK_SUCCESS) {
            _frameActive = false;
            recoverFailedFrameSubmission(beginResult);
            return;
        }

        // Advance the uniform ring to this frame's region.  The fence wait
        // above guarantees the GPU has finished reading it, so it is safe to
        // overwrite.  Lighting is re-packed into the fresh region on the next
        // draw (the prior frame's slot offset is now stale).
        if (_uniformRing) {
            _uniformRing->beginFrame(_frameIndex);
        }
        _lightingNeedsUpload = true;

        // Recycle all pools and cached sets owned by this frame slot. The
        // fence wait above guarantees none of them are still in GPU use.
        for (auto& pool : frame.descriptorPools) {
            if (pool.handle == VK_NULL_HANDLE) {
                continue;
            }
            const VkResult resetResult =
                vkResetDescriptorPool(_device, pool.handle, 0);
            if (resetResult == VK_SUCCESS) {
                continue;
            }

            // The frame fence makes destruction safe here. Replace a pool
            // that cannot be reset instead of handing its stale sets back to
            // the allocator.
            vkDestroyDescriptorPool(_device, pool.handle, nullptr);
            pool.handle = createFrameDescriptorPool(pool.maxSets);
            if (pool.handle == VK_NULL_HANDLE &&
                !_descriptorAllocationErrorWarned) {
                _descriptorAllocationErrorWarned = true;
                spdlog::error(
                    "VulkanGraphicsDevice: descriptor pool recycle failed ({})",
                    static_cast<int>(resetResult));
            }
        }
        frame.activeDescriptorPool = 0;
        frame.imageDescriptorCache.clear();
        _descriptorAllocationErrorWarned = false;
        _uniformOverflowWarned = false;
    }

    void VulkanGraphicsDevice::onFrameEnd()
    {
        if (!_frameActive) {
            // Frame was skipped at acquire time — nothing was recorded and the
            // fence was not reset, so there is nothing to submit or present.
            return;
        }
        _frameActive = false;

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        // Transition swapchain image → presentable, using whatever layout
        // the image was left in by the last render pass (or UNDEFINED if no
        // pass touched the swapchain this frame, which is common during
        // asset-load frames before anything is drawn).
        vulkanTransitionImageLayout(cmd, _swapchainImages[_swapchainImageIndex],
            _swapchainImageLayout,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        _swapchainImageLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        const VkResult endResult = vkEndCommandBuffer(cmd);
        if (endResult != VK_SUCCESS) {
            recoverFailedFrameSubmission(endResult);
            return;
        }

        VkSemaphore& renderFinished =
            _renderFinishedSemaphores[_swapchainImageIndex];

        // Upload batches use the same queue. Submitting them first gives the
        // frame an implicit queue-order dependency without blocking the CPU.
        flushUploads();

        // Submit
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAvailable;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &renderFinished;
        // Keep the fence signaled throughout command recording. Reset it only
        // at the last possible moment, when a submission is ready. If reset or
        // submit fails, recovery restores a signaled fence before slot reuse.
        const VkResult resetFenceResult =
            vkResetFences(_device, 1, &frame.inFlightFence);
        if (resetFenceResult != VK_SUCCESS) {
            recoverFailedFrameSubmission(resetFenceResult);
            return;
        }
        VkResult submitResult = VK_SUCCESS;
        if (_submitResultOverride) {
            submitResult = *_submitResultOverride;
            _submitResultOverride.reset();
        } else {
            submitResult = vkQueueSubmit(
                _graphicsQueue, 1, &submitInfo, frame.inFlightFence);
        }
        if (submitResult != VK_SUCCESS) {
            recoverFailedFrameSubmission(submitResult);
            return;
        }

        // Present
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &_swapchain;
        presentInfo.pImageIndices = &_swapchainImageIndex;
        const VkResult presentResult =
            vkQueuePresentKHR(_presentQueue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
            // Present is the usual place a resize surfaces — rebuild now so the
            // next acquire starts from a valid swapchain.
            (void)recreateSwapchain();
        } else if (presentResult != VK_SUCCESS) {
            spdlog::warn("VulkanGraphicsDevice: vkQueuePresentKHR failed ({})",
                static_cast<int>(presentResult));
        }

        ++_frameNumber;
        _frameIndex = (_frameIndex + 1) % kMaxFramesInFlight;
    }

    // ─────────────────────────────────────────────────────────────────────
    // VSM separable gaussian blur (fullscreen draw inside the active pass)
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::ensureVsmBlurResources()
    {
        if (_vsmBlurPipelineLayout != VK_NULL_HANDLE ||
            _vsmBlurResourcesAttempted) {
            return;
        }
        _vsmBlurResourcesAttempted = true;

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 1;
        setInfo.pBindings = &binding;
        VkResult result = vkCreateDescriptorSetLayout(
            _device, &setInfo, nullptr, &_vsmBlurSetLayout);
        if (result != VK_SUCCESS) {
            spdlog::error(
                "VulkanGraphicsDevice: VSM blur descriptor set layout creation failed ({})",
                static_cast<int>(result));
            return;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = 32; // vec4 dirInvRes + vec4 filterParams

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &_vsmBlurSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        result = vkCreatePipelineLayout(
            _device, &layoutInfo, nullptr, &_vsmBlurPipelineLayout);
        if (result != VK_SUCCESS) {
            spdlog::error(
                "VulkanGraphicsDevice: VSM blur pipeline layout creation failed ({})",
                static_cast<int>(result));
            vkDestroyDescriptorSetLayout(
                _device, _vsmBlurSetLayout, nullptr);
            _vsmBlurSetLayout = VK_NULL_HANDLE;
            return;
        }

        auto createModule = [this](
                                const uint32_t* spirv,
                                size_t words) -> VkShaderModule {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = words * sizeof(uint32_t);
            info.pCode = spirv;
            VkShaderModule module = VK_NULL_HANDLE;
            const VkResult moduleResult =
                vkCreateShaderModule(_device, &info, nullptr, &module);
            if (moduleResult != VK_SUCCESS) {
                spdlog::error(
                    "VulkanGraphicsDevice: VSM blur shader module creation failed ({})",
                    static_cast<int>(moduleResult));
                return VK_NULL_HANDLE;
            }
            return module;
        };
        _vsmBlurVertModule = createModule(
            vulkan_generated::kVsmBlurVert,
            vulkan_generated::kVsmBlurVertWordCount);
        _vsmBlurFragModule = createModule(
            vulkan_generated::kVsmBlurFrag,
            vulkan_generated::kVsmBlurFragWordCount);
        if (_vsmBlurVertModule == VK_NULL_HANDLE ||
            _vsmBlurFragModule == VK_NULL_HANDLE) {
            if (_vsmBlurVertModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(
                    _device, _vsmBlurVertModule, nullptr);
            }
            if (_vsmBlurFragModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(
                    _device, _vsmBlurFragModule, nullptr);
            }
            vkDestroyPipelineLayout(
                _device, _vsmBlurPipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(
                _device, _vsmBlurSetLayout, nullptr);
            _vsmBlurVertModule = VK_NULL_HANDLE;
            _vsmBlurFragModule = VK_NULL_HANDLE;
            _vsmBlurPipelineLayout = VK_NULL_HANDLE;
            _vsmBlurSetLayout = VK_NULL_HANDLE;
        }
    }

    VkPipeline VulkanGraphicsDevice::getVsmBlurPipeline(const VkFormat colorFormat, const VkFormat depthFormat)
    {
        const uint64_t key = (static_cast<uint64_t>(colorFormat) << 32) |
                             static_cast<uint64_t>(depthFormat);
        if (const auto it = _vsmBlurPipelines.find(key); it != _vsmBlurPipelines.end()) {
            return it->second;
        }
        if (_vsmBlurVertModule == VK_NULL_HANDLE || _vsmBlurFragModule == VK_NULL_HANDLE) {
            return VK_NULL_HANDLE;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = _vsmBlurVertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = _vsmBlurFragModule;
        stages[1].pName = "main";

        // Fullscreen triangle: no vertex input.
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Fullscreen blit — depth untouched even if the pass carries a depth attachment.
        VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;

        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkPipelineRenderingCreateInfo renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &colorFormat;
        renderingInfo.depthAttachmentFormat = depthFormat;

        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = _vsmBlurPipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
            spdlog::error("VulkanGraphicsDevice: VSM blur pipeline creation failed");
        }
        _vsmBlurPipelines[key] = pipeline;
        return pipeline;
    }

    void VulkanGraphicsDevice::executeVsmBlurPass(const VsmBlurPassParams& params, const bool horizontal)
    {
        if (!_frameActive || !_dynamicRenderingActive || !params.sourceTexture) {
            return;
        }
        auto* sourceTex = static_cast<gpu::VulkanTexture*>(params.sourceTexture->impl());
        if (!sourceTex || sourceTex->imageView() == VK_NULL_HANDLE) {
            return;
        }

        ensureVsmBlurResources();

        // Formats of the active pass (same derivation as draw()).
        VkFormat colorFmt = _swapchainFormat;
        VkFormat depthFmt = _depthFormat;
        if (_activeOffscreenTarget) {
            const auto& colors = _activeOffscreenTarget->colorAttachments();
            colorFmt = colors.empty() ? VK_FORMAT_UNDEFINED : colors[0].format;
            depthFmt = _activeOffscreenTarget->hasDepthAttachment()
                ? _activeOffscreenTarget->depthAttachment().format
                : VK_FORMAT_UNDEFINED;
        }
        if (colorFmt == VK_FORMAT_UNDEFINED) {
            return;
        }

        VkPipeline pipeline = getVsmBlurPipeline(colorFmt, depthFmt);
        if (pipeline == VK_NULL_HANDLE) {
            return;
        }

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = sourceTex->sampler() != VK_NULL_HANDLE ? sourceTex->sampler() : _defaultSampler;
        imageInfo.imageView = sourceTex->imageView();
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        const VkDescriptorSet set = getOrCreateImageDescriptorSet(
            _vsmBlurSetLayout, std::span(&imageInfo, 1));
        if (set == VK_NULL_HANDLE) {
            return;
        }

        struct { float dirInvRes[4]; float filterParams[4]; } push{};
        push.dirInvRes[0] = horizontal ? 1.0f : 0.0f;
        push.dirInvRes[1] = horizontal ? 0.0f : 1.0f;
        push.dirInvRes[2] = params.sourceInvResolutionX;
        push.dirInvRes[3] = params.sourceInvResolutionY;
        push.filterParams[0] = static_cast<float>(params.filterSize);
        push.filterParams[1] = params.tileSize;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            _vsmBlurPipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, _vsmBlurPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        // The blur pipeline replaced the material pipeline — force a rebind
        // (and full descriptor rebind via the incompatible layout) next draw.
        _currentPipeline = VK_NULL_HANDLE;
    }

    void VulkanGraphicsDevice::deferDestroy(std::function<void()> destroyFn)
    {
        if (!destroyFn) {
            return;
        }
        _deferredDestroys.push_back({_frameNumber, std::move(destroyFn)});
    }

    void VulkanGraphicsDevice::enqueueUpload(
        std::function<void(VkCommandBuffer)> record,
        std::function<void()> retire)
    {
        if (!record) {
            return;
        }
        std::lock_guard lock(_uploadMutex);
        _pendingUploads.push_back({std::move(record), std::move(retire)});
    }

    void VulkanGraphicsDevice::flushUploads()
    {
        std::vector<PendingUpload> uploads;
        {
            std::lock_guard lock(_uploadMutex);
            if (_pendingUploads.empty()) {
                return;
            }
            uploads.swap(_pendingUploads);
        }

        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = _uploadCommandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(_device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            spdlog::error("VulkanGraphicsDevice: failed to allocate upload command buffer");
            for (auto& upload : uploads) {
                if (upload.retire) upload.retire();
            }
            return;
        }

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        std::vector<std::function<void()>> retirements;
        retirements.reserve(uploads.size());
        for (auto& upload : uploads) {
            upload.record(commandBuffer);
            if (upload.retire) {
                retirements.push_back(std::move(upload.retire));
            }
        }
        vkEndCommandBuffer(commandBuffer);

        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        if (vkCreateFence(_device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
            vkFreeCommandBuffers(_device, _uploadCommandPool, 1, &commandBuffer);
            for (auto& retire : retirements) retire();
            return;
        }

        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(_graphicsQueue, 1, &submitInfo, fence) != VK_SUCCESS) {
            vkDestroyFence(_device, fence, nullptr);
            vkFreeCommandBuffers(_device, _uploadCommandPool, 1, &commandBuffer);
            for (auto& retire : retirements) retire();
            return;
        }

        _inFlightUploads.push_back(
            {commandBuffer, fence, std::move(retirements)});
    }

    void VulkanGraphicsDevice::collectUploads(const bool wait)
    {
        // Queue submission order means batches retire from the front.
        while (!_inFlightUploads.empty()) {
            auto& batch = _inFlightUploads.front();
            VkResult status = VK_NOT_READY;
            if (wait) {
                status = vkWaitForFences(
                    _device, 1, &batch.fence, VK_TRUE, UINT64_MAX);
            } else {
                status = vkGetFenceStatus(_device, batch.fence);
            }
            if (status != VK_SUCCESS) {
                break;
            }

            for (auto& retire : batch.retirements) {
                retire();
            }
            vkDestroyFence(_device, batch.fence, nullptr);
            vkFreeCommandBuffers(
                _device, _uploadCommandPool, 1, &batch.commandBuffer);
            _inFlightUploads.pop_front();
        }
    }

    void VulkanGraphicsDevice::flushDeferredDestroys(const bool force)
    {
        while (!_deferredDestroys.empty()) {
            const auto& front = _deferredDestroys.front();
            // A resource queued during frame N may be referenced by frame N's
            // own command buffer and the kMaxFramesInFlight-1 earlier ones;
            // frame N's fence has provably signaled once _frameNumber reaches
            // N + kMaxFramesInFlight (single queue, in-order completion).
            if (!force && front.frame + kMaxFramesInFlight > _frameNumber) {
                break;
            }
            auto fn = std::move(_deferredDestroys.front().fn);
            _deferredDestroys.pop_front();
            fn();
        }
    }

    void VulkanGraphicsDevice::collectRetiredSwapchains(const bool force)
    {
        if (_retiredSwapchains.empty()) {
            return;
        }

        const auto ready = [this, force](const RetiredSwapchain& retired) {
            return force ||
                retired.frame + kMaxFramesInFlight <= _frameNumber;
        };
        if (!ready(_retiredSwapchains.front())) {
            return;
        }

        // Frame fences cover graphics use of the old image views and depth
        // buffer. Presentation is not covered by those fences, however: its
        // queue may still be consuming renderFinished. Wait only that queue,
        // and only when an old bundle is actually ready to be destroyed.
        if (!force) {
            const VkResult presentWaitResult =
                vkQueueWaitIdle(_presentQueue);
            if (presentWaitResult != VK_SUCCESS) {
                _renderingDisabled = true;
                spdlog::error(
                    "VulkanGraphicsDevice: presentation queue wait while "
                    "retiring a swapchain failed ({}); rendering disabled",
                    static_cast<int>(presentWaitResult));
                return;
            }
        }

        while (!_retiredSwapchains.empty() &&
               ready(_retiredSwapchains.front())) {
            RetiredSwapchain retired =
                std::move(_retiredSwapchains.front());
            _retiredSwapchains.pop_front();

            for (VkSemaphore semaphore :
                 retired.renderFinishedSemaphores) {
                if (semaphore != VK_NULL_HANDLE) {
                    vkDestroySemaphore(_device, semaphore, nullptr);
                }
            }
            for (VkImageView view : retired.imageViews) {
                if (view != VK_NULL_HANDLE) {
                    vkDestroyImageView(_device, view, nullptr);
                }
            }
            if (retired.depthImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(
                    _device, retired.depthImageView, nullptr);
            }
            if (retired.depthImage != VK_NULL_HANDLE) {
                vmaDestroyImage(
                    _vmaAllocator, retired.depthImage,
                    retired.depthAllocation);
            }
            if (retired.swapchain != VK_NULL_HANDLE) {
                vkDestroySwapchainKHR(
                    _device, retired.swapchain, nullptr);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Render pass (dynamic rendering, Vulkan 1.3)
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::startRenderPass(RenderPass* renderPass)
    {
        if (!_frameActive) {
            return; // frame skipped at acquire — command buffer is not recording
        }

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        auto* offscreen = renderPass
            ? dynamic_cast<VulkanRenderTarget*>(renderPass->renderTarget().get())
            : nullptr;

        // ── Resolve attachment views, formats, extents, and clear ops ──
        const std::vector<std::shared_ptr<ColorAttachmentOps>> emptyColorOps;
        const auto& colorArrayOps = renderPass
            ? renderPass->colorArrayOps()
            : emptyColorOps;
        auto dsOps = renderPass ? renderPass->depthStencilOps() : nullptr;


        std::vector<VkRenderingAttachmentInfo> colorInfos;
        VkRenderingAttachmentInfo depthInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingAttachmentInfo stencilInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        bool hasDepth = false;
        bool hasStencil = false;
        VkExtent2D extent{};

        if (offscreen) {
            // Offscreen: transition each attachment from its current layout
            // (typically SHADER_READ_ONLY from a previous pass, or UNDEFINED
            // on first use) into the appropriate attachment-optimal layout.
            extent = offscreen->extent();

            const auto& attachments = offscreen->colorAttachments();
            for (size_t colorIndex = 0;
                 colorIndex < attachments.size(); ++colorIndex) {
                const auto& att = attachments[colorIndex];
                if (!att.texture) continue;
                const auto colorOps = colorIndex < colorArrayOps.size()
                    ? colorArrayOps[colorIndex] : nullptr;
                const uint32_t mip = static_cast<uint32_t>(offscreen->mipLevel());
                const uint32_t layer = att.texture->arrayLayers() > 1
                    ? static_cast<uint32_t>(offscreen->face()) : 0u;
                const VkImageLayout from = att.texture->layout(mip, layer);
                if (from != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                    att.texture->transitionLayout(cmd,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        mip, 1, layer, 1);
                }

                VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                info.imageView = att.view;
                info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                if (colorOps && colorOps->clear) {
                    info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    info.clearValue.color = {{colorOps->clearValue.r, colorOps->clearValue.g,
                                              colorOps->clearValue.b, colorOps->clearValue.a}};
                } else {
                    info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                }
                colorInfos.push_back(info);
            }

            if (offscreen->hasDepthAttachment()) {
                hasDepth = true;
                const auto& da = offscreen->depthAttachment();
                hasStencil = vulkanFormatHasStencil(da.format);
                const VkImageAspectFlags depthAspect = VK_IMAGE_ASPECT_DEPTH_BIT |
                    (hasStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);

                // Source image + source layout differ between texture-backed
                // and internally-owned depth.
                VkImage depthImg = da.texture ? da.texture->image() : da.internalImage;
                const uint32_t depthMip = static_cast<uint32_t>(offscreen->mipLevel());
                const uint32_t depthLayer = da.texture && da.texture->arrayLayers() > 1
                    ? static_cast<uint32_t>(offscreen->face()) : 0u;
                VkImageLayout fromLayout = da.texture
                    ? da.texture->layout(depthMip, depthLayer)
                    : da.currentLayout;
                if (depthImg != VK_NULL_HANDLE &&
                    fromLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                    // Omni-shadow cubemap depth carves per-face attachment views —
                    // barrier the face being rendered, not just layer 0 (the
                    // default), or faces 1-5 render in the wrong layout.
                    if (da.texture) {
                        da.texture->transitionLayout(cmd,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            depthMip, 1, depthLayer, 1);
                    } else {
                        vulkanTransitionImageLayout(cmd, depthImg,
                            fromLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                            depthAspect, 0, 1, 0, 1);
                        // Internal depth — track via the RT itself.
                        const_cast<VulkanDepthAttachment&>(da).currentLayout =
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    }
                }

                depthInfo.imageView = da.view;
                depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                depthInfo.loadOp = (dsOps && dsOps->clearDepth)
                    ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                depthInfo.storeOp = (dsOps && dsOps->storeDepth)
                    ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                depthInfo.clearValue.depthStencil = {dsOps ? dsOps->clearDepthValue : 1.0f, 0};

                if (hasStencil) {
                    stencilInfo = depthInfo;
                    stencilInfo.loadOp = (dsOps && dsOps->clearStencil)
                        ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                    stencilInfo.storeOp = (dsOps && dsOps->storeStencil)
                        ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
                    stencilInfo.clearValue.depthStencil.stencil =
                        static_cast<uint32_t>(dsOps ? dsOps->clearStencilValue : 0);
                }
            }
        } else {
            // Swapchain (back-buffer) path.
            extent = _swapchainExtent;
            const auto colorOps =
                colorArrayOps.empty() ? nullptr : colorArrayOps[0];

            if (_swapchainImageLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
                vulkanTransitionImageLayout(cmd, _swapchainImages[_swapchainImageIndex],
                    _swapchainImageLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                _swapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            // Transition the shared depth image from its tracked layout — a
            // blanket UNDEFINED source would discard contents even when a
            // second swapchain pass in the same frame (overlays/gizmos)
            // requests LOAD_OP_LOAD on depth.
            if (_depthImageLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
                vulkanTransitionImageLayout(cmd, _depthImage,
                    _depthImageLayout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_ASPECT_DEPTH_BIT);
                _depthImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }

            VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            info.imageView = _swapchainImageViews[_swapchainImageIndex];
            info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (colorOps && colorOps->clear) {
                info.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                info.clearValue.color = {{colorOps->clearValue.r, colorOps->clearValue.g,
                                          colorOps->clearValue.b, colorOps->clearValue.a}};
            } else {
                info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            }
            colorInfos.push_back(info);

            hasDepth = true;
            depthInfo.imageView = _depthImageView;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthInfo.loadOp = (dsOps && !dsOps->clearDepth)
                ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthInfo.storeOp = (dsOps && dsOps->storeDepth)
                ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthInfo.clearValue.depthStencil = {dsOps ? dsOps->clearDepthValue : 1.0f, 0};
        }

        VkRenderingInfo renderingInfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        renderingInfo.renderArea = {{0, 0}, extent};
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorInfos.size());
        renderingInfo.pColorAttachments = colorInfos.empty() ? nullptr : colorInfos.data();
        renderingInfo.pDepthAttachment = hasDepth ? &depthInfo : nullptr;
        renderingInfo.pStencilAttachment = hasStencil ? &stencilInfo : nullptr;

        vkCmdBeginRendering(cmd, &renderingInfo);

        _activeOffscreenTarget = offscreen;
        _activeExtent = extent;
        _dynamicRenderingActive = true;
        _insideRenderPass = true;
        _currentPipeline = VK_NULL_HANDLE;
        _pushConstantsDirty = true;

        // Depth-only offscreen passes are shadow-map renders. They use the SAME
        // negative-height viewport as every other pass: the shadow sample
        // matrices (shadowMatrixPalette) bake the Metal top-left atlas
        // orientation, and the negative-height viewport stores the map in
        // exactly that orientation — so the sampling shader needs no V flip.
        // (_depthOnlyPass only drives the white-texture descriptor fallbacks.)
        _depthOnlyPass = offscreen && colorInfos.empty() && hasDepth;

        // Every pass starts with a full-target viewport/scissor — same
        // contract as the Metal backend, which resets both at encoder
        // creation.  Camera rects / gizmo viewports are applied afterwards
        // through the setViewport/setScissor overrides.
        GraphicsDevice::setViewport(0.0f, 0.0f,
            static_cast<float>(extent.width), static_cast<float>(extent.height));
        GraphicsDevice::setScissor(0, 0,
            static_cast<int>(extent.width), static_cast<int>(extent.height));
        applyViewport();
        applyScissor();
        applyDepthBias();
    }

    void VulkanGraphicsDevice::endRenderPass(RenderPass* renderPass)
    {
        if (!_dynamicRenderingActive) {
            _insideRenderPass = false;
            return;
        }

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;
        vkCmdEndRendering(cmd);
        _dynamicRenderingActive = false;

        // Offscreen attachments are usually sampled by a later pass — transition
        // each one back to SHADER_READ_ONLY so the descriptor binding that the
        // next pass writes is valid.  Layout tracking on the texture (or on the
        // RT for internal depth) makes future transitions cheap.
        if (_activeOffscreenTarget) {
            const std::vector<std::shared_ptr<ColorAttachmentOps>> emptyColorOps;
            const auto& colorArrayOps = renderPass
                ? renderPass->colorArrayOps()
                : emptyColorOps;
            const auto& attachments =
                _activeOffscreenTarget->colorAttachments();
            for (size_t colorIndex = 0;
                 colorIndex < attachments.size(); ++colorIndex) {
                const auto& att = attachments[colorIndex];
                if (!att.texture) continue;
                const auto colorOps = colorIndex < colorArrayOps.size()
                    ? colorArrayOps[colorIndex] : nullptr;
                const uint32_t mip =
                    static_cast<uint32_t>(_activeOffscreenTarget->mipLevel());
                const uint32_t layer = att.texture->arrayLayers() > 1
                    ? static_cast<uint32_t>(_activeOffscreenTarget->face()) : 0u;
                att.texture->transitionLayout(cmd,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    mip, 1, layer, 1);
                if (mip == 0 && colorOps && colorOps->genMipmaps) {
                    att.texture->generateMipmaps(cmd);
                }
            }
            if (_activeOffscreenTarget->hasDepthAttachment()) {
                const auto& da = _activeOffscreenTarget->depthAttachment();
                // Only texture-backed depth can be sampled later — it was
                // created with SAMPLED_BIT.  Internal depth lives only inside
                // the render pass and stays in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                // until the next pass that uses it (which will start with that
                // same layout).  Transitioning internal depth to
                // SHADER_READ_ONLY is illegal because its image lacks
                // SAMPLED_BIT (VUID-VkImageMemoryBarrier-oldLayout-01211).
                if (da.texture && da.texture->image() != VK_NULL_HANDLE) {
                    // Mirror the per-face handling in startRenderPass for
                    // layered (omni cubemap) depth textures.
                    const bool depthLayered = da.texture->arrayLayers() > 1;
                    da.texture->transitionLayout(cmd,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        static_cast<uint32_t>(_activeOffscreenTarget->mipLevel()), 1,
                        depthLayered
                            ? static_cast<uint32_t>(_activeOffscreenTarget->face()) : 0u,
                        1u);
                }
            }
            _activeOffscreenTarget = nullptr;
        }

        _insideRenderPass = false;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Dynamic state (viewport / scissor / depth bias)
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::setViewport(const float x, const float y, const float w, const float h)
    {
        GraphicsDevice::setViewport(x, y, w, h);
        if (_dynamicRenderingActive) {
            applyViewport();
        }
    }

    void VulkanGraphicsDevice::setScissor(const int x, const int y, const int w, const int h)
    {
        GraphicsDevice::setScissor(x, y, w, h);
        if (_dynamicRenderingActive) {
            applyScissor();
        }
    }

    void VulkanGraphicsDevice::setDepthBias(const float depthBias, const float slopeScale, const float clamp)
    {
        _depthBiasConstant = depthBias;
        _depthBiasSlope = slopeScale;
        _depthBiasClamp = clamp;
        if (_dynamicRenderingActive) {
            applyDepthBias();
        }
    }

    void VulkanGraphicsDevice::applyViewport()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;

        const float w = vw() > 0.0f ? vw() : static_cast<float>(_activeExtent.width);
        const float h = vh() > 0.0f ? vh() : static_cast<float>(_activeExtent.height);

        // The engine uses a top-left-origin viewport (Metal convention).
        // Vulkan's normal viewport maps NDC +Y downwards; placing the origin
        // on the bottom edge of the rect and negating the height flips it so
        // projection matrices written for Metal/GL work unchanged. This applies
        // to shadow (depth-only) passes too: it stores shadow maps in the Metal
        // orientation the sample matrices bake (see startRenderPass).
        VkViewport viewport{};
        viewport.x = vx();
        viewport.y = vy() + h;
        viewport.width = w;
        viewport.height = -h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
    }

    void VulkanGraphicsDevice::applyScissor()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;

        const int64_t requestedWidth = sw() > 0
            ? static_cast<int64_t>(sw())
            : static_cast<int64_t>(_activeExtent.width);
        const int64_t requestedHeight = sh() > 0
            ? static_cast<int64_t>(sh())
            : static_cast<int64_t>(_activeExtent.height);
        const int64_t left = std::clamp<int64_t>(sx(), 0, _activeExtent.width);
        const int64_t top = std::clamp<int64_t>(sy(), 0, _activeExtent.height);
        const int64_t right = std::clamp<int64_t>(
            static_cast<int64_t>(sx()) + requestedWidth, left, _activeExtent.width);
        const int64_t bottom = std::clamp<int64_t>(
            static_cast<int64_t>(sy()) + requestedHeight, top, _activeExtent.height);

        VkRect2D scissor{};
        scissor.offset = {
            static_cast<int32_t>(left),
            static_cast<int32_t>(top)
        };
        scissor.extent = {
            static_cast<uint32_t>(right - left),
            static_cast<uint32_t>(bottom - top)
        };
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void VulkanGraphicsDevice::applyDepthBias()
    {
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;
        vkCmdSetDepthBias(cmd, _depthBiasConstant, _depthBiasClamp, _depthBiasSlope);
    }

    void VulkanGraphicsDevice::destroyComputeResources()
    {
        for (auto& [_, resources] : _computePipelines) {
            if (resources.pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(_device, resources.pipeline, nullptr);
            if (resources.pipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(_device, resources.pipelineLayout, nullptr);
            if (resources.setLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(_device, resources.setLayout, nullptr);
        }
        _computePipelines.clear();
        if (_particleSimPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(_device, _particleSimPipeline, nullptr);
        if (_particleSimPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(_device, _particleSimPipelineLayout, nullptr);
        if (_particleSimSetLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(_device, _particleSimSetLayout, nullptr);
    }

    bool VulkanGraphicsDevice::ensureParticleSimResources()
    {
        if (_particleSimPipeline != VK_NULL_HANDLE) return true;
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
        }};
        VkDescriptorSetLayoutCreateInfo setInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = bindings.size();
        setInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(_device, &setInfo, nullptr,
                &_particleSimSetLayout) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &_particleSimSetLayout;
        if (vkCreatePipelineLayout(_device, &layoutInfo, nullptr,
                &_particleSimPipelineLayout) != VK_SUCCESS) return false;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = vulkan_generated::kParticleSimCompWordCount * sizeof(uint32_t);
        moduleInfo.pCode = vulkan_generated::kParticleSimComp;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(_device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
            return false;
        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = _particleSimPipelineLayout;
        const VkResult result = vkCreateComputePipelines(_device, VK_NULL_HANDLE,
            1, &pipelineInfo, nullptr, &_particleSimPipeline);
        vkDestroyShaderModule(_device, module, nullptr);
        return result == VK_SUCCESS;
    }

    void VulkanGraphicsDevice::simulateParticles(
        const std::shared_ptr<VertexBuffer>& particles,
        const GpuParticleSimParams& params)
    {
        auto vkParticles = std::dynamic_pointer_cast<VulkanVertexBuffer>(particles);
        if (!vkParticles || !vkParticles->buffer() ||
            params.timeParams[3] <= 0.0f || !ensureParticleSimResources()) return;

        VkBuffer paramsBuffer = VK_NULL_HANDLE;
        VmaAllocation paramsAllocation = VK_NULL_HANDLE;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = sizeof(params);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo mappedInfo{};
        if (vmaCreateBuffer(_vmaAllocator, &bufferInfo, &allocationInfo,
                &paramsBuffer, &paramsAllocation, &mappedInfo) != VK_SUCCESS) return;
        std::memcpy(mappedInfo.pMappedData, &params, sizeof(params));
        vmaFlushAllocation(_vmaAllocator, paramsAllocation, 0, sizeof(params));

        std::array<VkDescriptorPoolSize, 2> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}
        }};
        VkDescriptorPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = sizes.size();
        poolInfo.pPoolSizes = sizes.data();
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(_device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
            vmaDestroyBuffer(_vmaAllocator, paramsBuffer, paramsAllocation);
            return;
        }
        VkDescriptorSetAllocateInfo setAlloc{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAlloc.descriptorPool = pool;
        setAlloc.descriptorSetCount = 1;
        setAlloc.pSetLayouts = &_particleSimSetLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(_device, &setAlloc, &set) != VK_SUCCESS) {
            vkDestroyDescriptorPool(_device, pool, nullptr);
            vmaDestroyBuffer(_vmaAllocator, paramsBuffer, paramsAllocation);
            return;
        }
        std::array<VkDescriptorBufferInfo, 2> infos{{
            {vkParticles->buffer(), 0, VK_WHOLE_SIZE},
            {paramsBuffer, 0, sizeof(params)}
        }};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = sizes[i].type;
            writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(_device, writes.size(), writes.data(), 0, nullptr);
        const VkPipeline pipeline = _particleSimPipeline;
        const VkPipelineLayout layout = _particleSimPipelineLayout;
        const VkBuffer particleBuffer = vkParticles->buffer();
        const uint32_t groups =
            (static_cast<uint32_t>(params.timeParams[3]) + 255u) / 256u;
        enqueueUpload(
            [pipeline, layout, set, particleBuffer, groups,
             keepAlive = std::move(vkParticles)](VkCommandBuffer cmd) {
                (void)keepAlive;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    layout, 0, 1, &set, 0, nullptr);
                vkCmdDispatch(cmd, groups, 1, 1);
                VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.buffer = particleBuffer;
                barrier.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr,
                    1, &barrier, 0, nullptr);
            },
            [device = _device, allocator = _vmaAllocator, pool,
             paramsBuffer, paramsAllocation] {
                vkDestroyDescriptorPool(device, pool, nullptr);
                vmaDestroyBuffer(allocator, paramsBuffer, paramsAllocation);
            });
        flushUploads();
    }

    void VulkanGraphicsDevice::setParticleState(
        const std::shared_ptr<VertexBuffer>& particles,
        const void* params, size_t paramsSize)
    {
        if (!particles || !params || paramsSize == 0 ||
            paramsSize > _pendingParticleParams.size()) {
            _pendingParticleBuffer.reset();
            _pendingParticleParamsSize = 0;
            return;
        }
        _pendingParticleBuffer = particles;
        std::memcpy(_pendingParticleParams.data(), params, paramsSize);
        _pendingParticleParamsSize = paramsSize;
    }

    void VulkanGraphicsDevice::setGSplatState(
        const std::shared_ptr<VertexBuffer>& splats,
        const std::shared_ptr<VertexBuffer>& order,
        const std::shared_ptr<VertexBuffer>& sh,
        const void* params, size_t paramsSize)
    {
        if (!splats || !order || !params || paramsSize == 0 ||
            paramsSize > _pendingGSplatParams.size()) {
            _pendingGSplatBuffer.reset();
            _pendingGSplatOrderBuffer.reset();
            _pendingGSplatShBuffer.reset();
            _pendingGSplatParamsSize = 0;
            return;
        }
        _pendingGSplatBuffer = splats;
        _pendingGSplatOrderBuffer = order;
        _pendingGSplatShBuffer = sh;
        std::memcpy(_pendingGSplatParams.data(), params, paramsSize);
        _pendingGSplatParamsSize = paramsSize;
    }

    void VulkanGraphicsDevice::computeDispatch(
        const std::vector<Compute*>& computes, const std::string& label)
    {
        (void)label;
        if (_dynamicRenderingActive) {
            spdlog::warn("Vulkan compute dispatch cannot run inside a render pass");
            return;
        }

        for (Compute* compute : computes) {
            if (!compute || !compute->shader()) continue;
            auto shader = std::dynamic_pointer_cast<VulkanShader>(compute->shader());
            if (!shader || shader->computeModule() == VK_NULL_HANDLE) {
                spdlog::error("Vulkan compute dispatch '{}' has no compute module",
                    compute->name());
                continue;
            }

            std::vector<std::pair<std::string, Texture*>> parameters(
                compute->textureParameters().begin(),
                compute->textureParameters().end());
            std::ranges::sort(parameters, {}, &decltype(parameters)::value_type::first);

            std::vector<gpu::VulkanTexture*> textures;
            std::vector<VkDescriptorType> types;
            bool valid = true;
            for (const auto& [name, texture] : parameters) {
                (void)name;
                if (texture) {
                    auto* pendingTexture =
                        dynamic_cast<gpu::VulkanTexture*>(texture->impl());
                    if (!pendingTexture ||
                        pendingTexture->image() == VK_NULL_HANDLE) {
                        texture->upload();
                    }
                }
                auto* vkTexture = texture
                    ? dynamic_cast<gpu::VulkanTexture*>(texture->impl()) : nullptr;
                if (!vkTexture || vkTexture->image() == VK_NULL_HANDLE) {
                    valid = false;
                    break;
                }
                textures.push_back(vkTexture);
                types.push_back(texture->storage()
                    ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                    : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            }
            if (!valid || textures.empty()) {
                spdlog::error("Vulkan compute dispatch '{}' has invalid texture parameters",
                    compute->name());
                continue;
            }

            auto [pipelineIt, inserted] = _computePipelines.try_emplace(shader->id());
            auto& resources = pipelineIt->second;
            if (inserted) {
                resources.descriptorTypes = types;
                std::vector<VkDescriptorSetLayoutBinding> bindings(types.size());
                for (uint32_t i = 0; i < bindings.size(); ++i) {
                    bindings[i] = {i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
                }
                VkDescriptorSetLayoutCreateInfo setInfo{
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
                setInfo.pBindings = bindings.data();
                if (vkCreateDescriptorSetLayout(_device, &setInfo, nullptr,
                        &resources.setLayout) != VK_SUCCESS) {
                    spdlog::error("Failed to create Vulkan compute descriptor layout");
                    _computePipelines.erase(pipelineIt);
                    continue;
                }
                VkPipelineLayoutCreateInfo layoutInfo{
                    VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                layoutInfo.setLayoutCount = 1;
                layoutInfo.pSetLayouts = &resources.setLayout;
                if (vkCreatePipelineLayout(_device, &layoutInfo, nullptr,
                        &resources.pipelineLayout) != VK_SUCCESS) {
                    vkDestroyDescriptorSetLayout(_device, resources.setLayout, nullptr);
                    _computePipelines.erase(pipelineIt);
                    continue;
                }
                VkPipelineShaderStageCreateInfo stage{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                stage.module = shader->computeModule();
                stage.pName = "main";
                VkComputePipelineCreateInfo pipelineInfo{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                pipelineInfo.stage = stage;
                pipelineInfo.layout = resources.pipelineLayout;
                if (vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1,
                        &pipelineInfo, nullptr, &resources.pipeline) != VK_SUCCESS) {
                    vkDestroyPipelineLayout(_device, resources.pipelineLayout, nullptr);
                    vkDestroyDescriptorSetLayout(_device, resources.setLayout, nullptr);
                    _computePipelines.erase(pipelineIt);
                    continue;
                }
            } else if (resources.descriptorTypes != types) {
                spdlog::error(
                    "Vulkan compute shader '{}' was rebound with an incompatible resource layout",
                    compute->name());
                continue;
            }

            std::vector<VkDescriptorPoolSize> poolSizes;
            for (VkDescriptorType type : types) {
                const auto it = std::ranges::find_if(poolSizes,
                    [type](const VkDescriptorPoolSize& size) { return size.type == type; });
                if (it == poolSizes.end()) poolSizes.push_back({type, 1});
                else ++it->descriptorCount;
            }
            VkDescriptorPoolCreateInfo poolInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1;
            poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(_device, &poolInfo, nullptr,
                    &descriptorPool) != VK_SUCCESS) continue;
            VkDescriptorSetAllocateInfo allocInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocInfo.descriptorPool = descriptorPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &resources.setLayout;
            VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(_device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
                vkDestroyDescriptorPool(_device, descriptorPool, nullptr);
                continue;
            }

            std::vector<VkDescriptorImageInfo> imageInfos(textures.size());
            std::vector<VkWriteDescriptorSet> writes(textures.size());
            for (uint32_t i = 0; i < textures.size(); ++i) {
                const bool storage = types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                imageInfos[i].sampler = storage ? VK_NULL_HANDLE : textures[i]->sampler();
                imageInfos[i].imageView = textures[i]->imageView();
                imageInfos[i].imageLayout = storage
                    ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = descriptorSet;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = types[i];
                writes[i].pImageInfo = &imageInfos[i];
            }
            vkUpdateDescriptorSets(_device, static_cast<uint32_t>(writes.size()),
                writes.data(), 0, nullptr);

            const VkPipeline pipeline = resources.pipeline;
            const VkPipelineLayout pipelineLayout = resources.pipelineLayout;
            const uint32_t dispatchX = compute->dispatchX();
            const uint32_t dispatchY = compute->dispatchY();
            const uint32_t dispatchZ = compute->dispatchZ();
            enqueueUpload(
                [textures, types, descriptorSet, pipeline, pipelineLayout,
                 dispatchX, dispatchY, dispatchZ](VkCommandBuffer cmd) {
                    for (uint32_t i = 0; i < textures.size(); ++i) {
                        textures[i]->transitionLayout(cmd,
                            types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                ? VK_IMAGE_LAYOUT_GENERAL
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
                    vkCmdDispatch(cmd, dispatchX, dispatchY, dispatchZ);
                    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier,
                        0, nullptr, 0, nullptr);
                    for (uint32_t i = 0; i < textures.size(); ++i) {
                        if (types[i] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                            textures[i]->transitionLayout(cmd,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        }
                    }
                },
                [device = _device, descriptorPool] {
                    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
                });
        }
        flushUploads();
    }

    // ─────────────────────────────────────────────────────────────────────
    // Core rendering
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::draw(const Primitive& primitive,
        const std::shared_ptr<IndexBuffer>& indexBuffer,
        int numInstances, int indirectSlot, bool first, bool last)
    {
        if (!_shader || !_dynamicRenderingActive) return;

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        auto vulkanShader = std::dynamic_pointer_cast<VulkanShader>(_shader);
        if (!vulkanShader || vulkanShader->vertexModule() == VK_NULL_HANDLE) return;

        // Geometry bindings are deliberately one-shot, matching Metal. Move
        // them into draw-local state before any fallible pipeline/descriptor
        // work so an aborted draw cannot leak stale deformation state.
        const auto paletteOffset = _pendingPaletteOffset;
        const VkDeviceSize paletteSize = _pendingPaletteSize;
        auto morphDeltaBuffer = std::move(_pendingMorphDeltaBuffer);
        const auto morphParamsOffset = _pendingMorphParamsOffset;
        const VkDeviceSize morphParamsSize = _pendingMorphParamsSize;
        _pendingPaletteOffset.reset();
        _pendingPaletteSize = 0;
        _pendingMorphParamsOffset.reset();
        _pendingMorphParamsSize = 0;
        auto particleBuffer = std::move(_pendingParticleBuffer);
        const auto particleParams = _pendingParticleParams;
        const size_t particleParamsSize = _pendingParticleParamsSize;
        _pendingParticleParamsSize = 0;
        auto splatBuffer = std::move(_pendingGSplatBuffer);
        auto splatOrderBuffer = std::move(_pendingGSplatOrderBuffer);
        auto splatShBuffer = std::move(_pendingGSplatShBuffer);
        const auto splatParams = _pendingGSplatParams;
        const size_t splatParamsSize = _pendingGSplatParamsSize;
        _pendingGSplatParamsSize = 0;

        if (first) {
            auto vf = !_vertexBuffers.empty() ? _vertexBuffers[0] : nullptr;

            // Hardware instancing: the renderer binds the per-instance buffer
            // at engine slot 5 with an isInstancing() format (same contract
            // as the Metal backend).  Scan the upper slots for it.
            const VulkanVertexBuffer* instancingVB = nullptr;
            for (size_t i = 1; i < _vertexBuffers.size(); ++i) {
                if (_vertexBuffers[i] && _vertexBuffers[i]->format() &&
                    _vertexBuffers[i]->format()->isInstancing()) {
                    instancingVB = static_cast<VulkanVertexBuffer*>(_vertexBuffers[i].get());
                    break;
                }
            }
            const auto instanceFormat = instancingVB ? instancingVB->format() : nullptr;

            // Resolve attachment formats for pipeline creation.  The pipeline
            // is keyed on these — a mismatch with the actual VkRenderingInfo
            // attachments at draw-time is rejected by validation as
            // VUID-vkCmdDrawIndexed-dynamicRenderingUnusedAttachments-08910.
            std::vector<VkFormat> colorFormats{_swapchainFormat};
            VkFormat depthFmt = _depthFormat;
            if (_activeOffscreenTarget) {
                const auto& colors = _activeOffscreenTarget->colorAttachments();
                colorFormats.clear();
                colorFormats.reserve(colors.size());
                for (const auto& color : colors) {
                    colorFormats.push_back(color.format);
                }
                depthFmt = _activeOffscreenTarget->hasDepthAttachment()
                    ? _activeOffscreenTarget->depthAttachment().format
                    : VK_FORMAT_UNDEFINED;
            }

            // The skybox is an inward-facing shell whose authored winding,
            // combined with our negative-height (Y-flipped) viewport, makes
            // its CULLFACE_FRONT cull the visible inner faces.  Render it with
            // no culling so the environment shell is always drawn, and select
            // the depth-pin skybox vertex stage.
            const bool isSkybox = _material && _material->isSkybox();
            CullMode cullMode = isSkybox ? CullMode::CULLFACE_NONE : _cullMode;

            VkPipeline pipeline = _renderPipeline->get(primitive,
                vf ? vf->format() : nullptr,
                instanceFormat,
                vulkanShader, _blendState, _depthState, cullMode,
                _stencilEnabled, _stencilFront, _stencilBack,
                colorFormats, depthFmt, isSkybox);

            if (pipeline == VK_NULL_HANDLE) {
                spdlog::error("VulkanGraphicsDevice: draw skipped because pipeline creation failed");
                return;
            }
            if (pipeline != _currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                _currentPipeline = pipeline;
                _pushConstantsDirty = true;
            }

            // Bind vertex buffer
            if (vf) {
                auto* vb = static_cast<VulkanVertexBuffer*>(vf.get());
                if (vb->buffer() != VK_NULL_HANDLE) {
                    VkBuffer buf = vb->buffer();
                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
                }
            }

            // Bind per-instance buffer at binding 1 (matches the pipeline's
            // VK_VERTEX_INPUT_RATE_INSTANCE binding).
            if (instancingVB && instancingVB->buffer() != VK_NULL_HANDLE) {
                VkBuffer instBuf = instancingVB->buffer();
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 1, 1, &instBuf, &offset);
            }
        }

        if (_stencilEnabled && (_stencilFront || _stencilBack)) {
            const auto& effectiveFront = _stencilFront ? _stencilFront : _stencilBack;
            const auto& effectiveBack = _stencilBack ? _stencilBack : _stencilFront;
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT,
                effectiveFront->reference());
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT,
                effectiveBack->reference());
        } else {
            // Pipelines declare stencil reference as dynamic even when the
            // current attachment/state does not use stencil. Define it on
            // every command buffer so a first non-stencil draw never depends
            // on state left by an earlier draw or frame.
            vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        }

        // Push constants (transforms)
        if (_pushConstantsDirty) {
            vkCmdPushConstants(cmd, _renderPipeline->pipelineLayout(),
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &_pushConstants);
            _pushConstantsDirty = false;
        }

        // Set 2: per-pass lighting UBO.  Packed once per frame (or whenever
        // setLightingUniforms changed it) into the ring; every draw binds the
        // same descriptor set with the cached dynamic offset.
        if (_lightingNeedsUpload) {
            const auto lightingOffset =
                allocateUniform(&_lightingUbo, sizeof(VulkanLightingUBO));
            if (!lightingOffset) {
                return;
            }
            _lightingSlotOffset = *lightingOffset;
            _lightingNeedsUpload = false;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            _renderPipeline->pipelineLayout(), 2, 1, &_lightingDescriptorSet,
            1, &_lightingSlotOffset);

        // Set 0: per-draw material UBO.  Pack MaterialUniforms (or the
        // material's custom uniform block) into the ring and bind via the
        // dynamic offset.  The shader's MaterialData block is statically used,
        // so it MUST be bound (VUID-vkCmdDrawIndexed-None-08600).
        {
            MaterialUniforms materialUniforms;
            const void* uniformData = &materialUniforms;
            size_t uniformSize = sizeof(MaterialUniforms);
            if (_material) {
                size_t customSize = 0;
                const void* customData = _material->customUniformData(customSize);
                if (customData && customSize > 0) {
                    uniformData = customData;
                    uniformSize = customSize;
                } else {
                    _material->updateUniforms(materialUniforms);
                }
            }
            const auto matOffset = allocateUniform(uniformData, uniformSize);
            if (!matOffset) {
                return;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 0, 1, &_materialDescriptorSet,
                1, &*matOffset);
        }

        // Set 1: material textures. Cache identical image/sampler tuples for
        // the lifetime of this frame slot instead of allocating per draw.
        {
            constexpr std::array<int, 6> materialSlots = {0, 1, 3, 4, 5, 19};
            std::array<VkDescriptorImageInfo, materialSlots.size()> imageInfos{};
            for (auto& imageInfo : imageInfos) {
                imageInfo.sampler = _defaultSampler;
                imageInfo.imageView = _whiteImageView;
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            if (_material) {
                std::vector<TextureSlot> texSlots;
                _material->getTextureSlots(texSlots);
                for (const auto& ts : texSlots) {
                    const auto slotIt = std::find(
                        materialSlots.begin(), materialSlots.end(), ts.slot);
                    if (slotIt == materialSlots.end() || ts.texture == nullptr) {
                        continue;
                    }
                    auto* vkTex =
                        static_cast<gpu::VulkanTexture*>(ts.texture->impl());
                    if (vkTex && vkTex->imageView() != VK_NULL_HANDLE) {
                        const size_t descriptorIndex =
                            static_cast<size_t>(slotIt - materialSlots.begin());
                        imageInfos[descriptorIndex].imageView = vkTex->imageView();
                        if (vkTex->sampler() != VK_NULL_HANDLE) {
                            imageInfos[descriptorIndex].sampler = vkTex->sampler();
                        }
                    }
                }
            }

            const VkDescriptorSet texSet = getOrCreateImageDescriptorSet(
                _renderPipeline->textureSetLayout(), imageInfos);
            if (texSet == VK_NULL_HANDLE) {
                return;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 1, 1, &texSet, 0, nullptr);
        }

        // Set 3: scene textures.  Binding 0 = environment atlas (or white
        // fallback) read through the dedicated clamp-to-edge env sampler.
        {
            auto resolveView = [this](Texture* tex) -> VkImageView {
                if (tex) {
                    auto* vkTex =
                        static_cast<gpu::VulkanTexture*>(tex->impl());
                    if (vkTex && vkTex->imageView() != VK_NULL_HANDLE) {
                        return vkTex->imageView();
                    }
                }
                return _whiteImageView;
            };

            // Resolve a cube view, falling back to the white cubemap so an
            // unbound omni slot reads fully lit (and never mixes a 2D view
            // into a samplerCube descriptor, which is invalid).
            auto resolveCubeView = [this](Texture* tex) -> VkImageView {
                if (tex) {
                    auto* vkTex =
                        static_cast<gpu::VulkanTexture*>(tex->impl());
                    if (vkTex && vkTex->imageView() != VK_NULL_HANDLE) {
                        return vkTex->imageView();
                    }
                }
                return _whiteCubeImageView;
            };

            // During a shadow render the shadow map is the attachment being
            // written, so bind white fallbacks instead of creating feedback.
            bool shadowIsActiveAttachment = false;
            if (_activeOffscreenTarget && _shadowMapTexture &&
                !_activeOffscreenTarget->colorAttachments().empty()) {
                shadowIsActiveAttachment =
                    _activeOffscreenTarget->colorAttachments()[0].texture ==
                    static_cast<gpu::VulkanTexture*>(
                        _shadowMapTexture->impl());
            }
            const bool hideShadowMaps =
                _depthOnlyPass || shadowIsActiveAttachment;

            std::array<VkDescriptorImageInfo, 8> sceneInfos{};
            sceneInfos[0].sampler = _envSampler;
            sceneInfos[0].imageView = resolveView(_envAtlasTexture);

            sceneInfos[1].sampler = _shadowSampler;
            if (!hideShadowMaps && _shadowMapTexture) {
                if (auto* vkShadowTex =
                        static_cast<gpu::VulkanTexture*>(
                            _shadowMapTexture->impl());
                    vkShadowTex &&
                    vkShadowTex->sampler() != VK_NULL_HANDLE) {
                    sceneInfos[1].sampler = vkShadowTex->sampler();
                }
            }
            sceneInfos[1].imageView = hideShadowMaps
                ? _whiteImageView : resolveView(_shadowMapTexture);
            sceneInfos[2].sampler = _shadowSampler;
            sceneInfos[2].imageView = _depthOnlyPass
                ? _whiteImageView : resolveView(_localShadowTexture0);
            sceneInfos[3].sampler = _shadowSampler;
            sceneInfos[3].imageView = _depthOnlyPass
                ? _whiteImageView : resolveView(_localShadowTexture1);
            sceneInfos[4].sampler = _shadowSampler;
            sceneInfos[4].imageView = _depthOnlyPass
                ? _whiteCubeImageView : resolveCubeView(_omniShadowCube0);
            sceneInfos[5].sampler = _shadowSampler;
            sceneInfos[5].imageView = _depthOnlyPass
                ? _whiteCubeImageView : resolveCubeView(_omniShadowCube1);
            sceneInfos[6].sampler = _envSampler;
            sceneInfos[6].imageView = resolveCubeView(_skyboxCubeTexture);
            sceneInfos[7].sampler = _envSampler;
            sceneInfos[7].imageView = resolveCubeView(_reflectionProbeTexture);
            for (auto& sceneInfo : sceneInfos) {
                sceneInfo.imageLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            const VkDescriptorSet sceneSet = getOrCreateImageDescriptorSet(
                _renderPipeline->sceneSetLayout(), sceneInfos);
            if (sceneSet == VK_NULL_HANDLE) {
                return;
            }
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 3, 1, &sceneSet, 0, nullptr);
        }

        // Set 4: deformation data selected by the shared feature mask.
        const uint64_t featureMask = vulkanShader->featureMask();
        const bool usesPalette =
            (featureMask & (shaderFeatureBit(ShaderFeature::Skinning) |
                            shaderFeatureBit(ShaderFeature::DynamicBatch))) != 0;
        const bool usesMorph =
            (featureMask & shaderFeatureBit(ShaderFeature::Morphing)) != 0;
        if (usesPalette || usesMorph) {
            if ((usesPalette && (!paletteOffset || paletteSize == 0)) ||
                (usesMorph && (!morphDeltaBuffer || !morphParamsOffset ||
                               morphParamsSize == 0))) {
                spdlog::error(
                    "VulkanGraphicsDevice: draw skipped because required "
                    "palette/morph geometry state was not supplied");
                return;
            }

            const VkDescriptorSet geometrySet = allocateFrameDescriptorSet(
                _renderPipeline->geometrySetLayout());
            if (geometrySet == VK_NULL_HANDLE) {
                return;
            }

            std::array<VkDescriptorBufferInfo, 3> infos{};
            std::array<VkWriteDescriptorSet, 3> writes{};
            uint32_t writeCount = 0;
            auto appendBuffer = [&](const uint32_t binding,
                                    const VkDescriptorType type,
                                    const VkBuffer buffer,
                                    const VkDeviceSize offset,
                                    const VkDeviceSize range) {
                infos[writeCount] = {buffer, offset, range};
                auto& write = writes[writeCount];
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = geometrySet;
                write.dstBinding = binding;
                write.descriptorType = type;
                write.descriptorCount = 1;
                write.pBufferInfo = &infos[writeCount];
                ++writeCount;
            };
            if (usesPalette) {
                appendBuffer(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    _uniformRing->buffer(), *paletteOffset, paletteSize);
            }
            if (usesMorph) {
                auto* morphBuffer =
                    static_cast<VulkanVertexBuffer*>(morphDeltaBuffer.get());
                appendBuffer(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    morphBuffer->buffer(), 0, VK_WHOLE_SIZE);
                appendBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    _uniformRing->buffer(), *morphParamsOffset,
                    morphParamsSize);
            }
            vkUpdateDescriptorSets(_device, writeCount, writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 4, 1, &geometrySet, 0, nullptr);
        }

        {
            // Specialization constants do not remove statically-declared
            // descriptors from SPIR-V, so set 5 must be valid for every
            // forward draw. Non-clustered draws bind tiny zero sentinels.
            std::array<uint8_t, 144> emptyLight{};
            const uint32_t emptyCell = 0;
            const auto lightOffset = _clusterLightOffset
                ? _clusterLightOffset : allocateUniform(emptyLight.data(), emptyLight.size());
            const auto cellOffset = _clusterCellOffset
                ? _clusterCellOffset : allocateUniform(&emptyCell, sizeof(emptyCell));
            if (!lightOffset || !cellOffset) return;
            const VkDescriptorSet clusterSet = allocateFrameDescriptorSet(
                _renderPipeline->clusterSetLayout());
            if (clusterSet == VK_NULL_HANDLE) return;
            std::array<VkDescriptorBufferInfo, 2> infos{{
                {_uniformRing->buffer(), *lightOffset,
                    _clusterLightOffset ? _clusterLightSize : emptyLight.size()},
                {_uniformRing->buffer(), *cellOffset,
                    _clusterCellOffset ? _clusterCellSize : sizeof(emptyCell)},
            }};
            std::array<VkWriteDescriptorSet, 2> writes{};
            for (uint32_t i = 0; i < writes.size(); ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = clusterSet;
                writes[i].dstBinding = i;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].descriptorCount = 1;
                writes[i].pBufferInfo = &infos[i];
            }
            vkUpdateDescriptorSets(_device, writes.size(), writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 5, 1, &clusterSet, 0, nullptr);
        }

        // Set 6: dedicated GPU-driven render resources. The state is one-shot
        // and was moved above, so failed draws cannot leak it to later meshes.
        if (particleBuffer || splatBuffer) {
            const bool particleDraw = particleBuffer != nullptr;
            const void* paramsData = particleDraw
                ? static_cast<const void*>(particleParams.data())
                : static_cast<const void*>(splatParams.data());
            const size_t paramsSize = particleDraw
                ? particleParamsSize : splatParamsSize;
            auto paramsOffset = allocateUniform(paramsData, paramsSize);
            auto primary = std::dynamic_pointer_cast<VulkanVertexBuffer>(
                particleDraw ? particleBuffer : splatBuffer);
            auto order = std::dynamic_pointer_cast<VulkanVertexBuffer>(splatOrderBuffer);
            auto sh = std::dynamic_pointer_cast<VulkanVertexBuffer>(splatShBuffer);
            if (!paramsOffset || !primary || !primary->buffer() ||
                (!particleDraw && (!order || !order->buffer()))) {
                return;
            }
            const VkDescriptorSet gpuSet = allocateFrameDescriptorSet(
                _renderPipeline->gpuDrivenSetLayout());
            if (gpuSet == VK_NULL_HANDLE) return;
            std::array<VkDescriptorBufferInfo, 4> infos{};
            infos[0] = {primary->buffer(), 0, VK_WHOLE_SIZE};
            if (order) infos[1] = {order->buffer(), 0, VK_WHOLE_SIZE};
            if (sh) infos[2] = {sh->buffer(), 0, VK_WHOLE_SIZE};
            infos[3] = {_uniformRing->buffer(), *paramsOffset, paramsSize};
            std::array<VkWriteDescriptorSet, 4> writes{};
            uint32_t writeCount = 0;
            const auto addWrite = [&](uint32_t binding, VkDescriptorType type) {
                auto& write = writes[writeCount++];
                write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = gpuSet;
                write.dstBinding = binding;
                write.descriptorCount = 1;
                write.descriptorType = type;
                write.pBufferInfo = &infos[binding];
            };
            addWrite(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            if (!particleDraw) {
                addWrite(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                // Current Vulkan splat variant evaluates SH0 color. Keep the
                // SH buffer in state so higher-band evaluation can be enabled
                // without changing the public binding contract.
                if (sh) addWrite(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            }
            addWrite(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            vkUpdateDescriptorSets(_device, writeCount, writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                _renderPipeline->pipelineLayout(), 6, 1, &gpuSet, 0, nullptr);
        }

        // Draw
        if (indexBuffer) {
            auto* ib = static_cast<VulkanIndexBuffer*>(indexBuffer.get());
            if (ib->buffer() != VK_NULL_HANDLE) {
                vkCmdBindIndexBuffer(cmd, ib->buffer(), 0, ib->indexType());
                if (indirectSlot >= 0 && _indirectDrawBuffer != VK_NULL_HANDLE) {
                    vkCmdDrawIndexedIndirect(cmd, _indirectDrawBuffer,
                        static_cast<VkDeviceSize>(indirectSlot) *
                            sizeof(VkDrawIndexedIndirectCommand),
                        1, sizeof(VkDrawIndexedIndirectCommand));
                    _indirectDrawBuffer = VK_NULL_HANDLE;
                } else {
                    vkCmdDrawIndexed(cmd, primitive.count, numInstances,
                        primitive.base, primitive.baseVertex, 0);
                }
            }
        } else {
            if (indirectSlot >= 0 && _indirectDrawBuffer != VK_NULL_HANDLE) {
                vkCmdDrawIndirect(cmd, _indirectDrawBuffer,
                    static_cast<VkDeviceSize>(indirectSlot) *
                        sizeof(VkDrawIndirectCommand),
                    1, sizeof(VkDrawIndirectCommand));
                _indirectDrawBuffer = VK_NULL_HANDLE;
            } else {
                vkCmdDraw(cmd, primitive.count, numInstances, primitive.base, 0);
            }
        }

        recordDrawCall();

        if (last) {
            clearVertexBuffer();
            _currentPipeline = VK_NULL_HANDLE;
        }
    }

    void VulkanGraphicsDevice::grabSceneColor(RenderTarget* source)
    {
        if (!_frameActive || _dynamicRenderingActive) return;
        Texture* sourceTexture = source && source->colorBufferCount() > 0
            ? source->getColorBuffer(0) : nullptr;
        auto* src = sourceTexture
            ? dynamic_cast<gpu::VulkanTexture*>(sourceTexture->impl()) : nullptr;
        const uint32_t sourceWidth = sourceTexture
            ? sourceTexture->width() : _swapchainExtent.width;
        const uint32_t sourceHeight = sourceTexture
            ? sourceTexture->height() : _swapchainExtent.height;
        if (!src && _swapchainImageIndex >= _swapchainImages.size()) return;
        if (!_sceneColorGrabTexture ||
            _sceneColorGrabTexture->width() != sourceWidth ||
            _sceneColorGrabTexture->height() != sourceHeight) {
            TextureOptions options;
            options.name = "sceneColorGrab";
            options.width = sourceWidth;
            options.height = sourceHeight;
            options.format = sourceTexture
                ? sourceTexture->format() : PixelFormat::PIXELFORMAT_RGBA8;
            options.mipmaps = true;
            _sceneColorGrabTexture = std::make_shared<Texture>(this, options);
            _sceneColorGrabTexture->upload();
        }
        auto* dst = dynamic_cast<gpu::VulkanTexture*>(
            _sceneColorGrabTexture->impl());
        if (!dst) return;
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;
        VkImage sourceImage = src ? src->image() : _swapchainImages[_swapchainImageIndex];
        if (src) {
            src->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        } else {
            vulkanTransitionImageLayout(cmd, sourceImage, _swapchainImageLayout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            _swapchainImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        dst->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {sourceWidth, sourceHeight, 1};
        vkCmdCopyImage(cmd, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (src) {
            src->transitionLayout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 0, 1);
        } else {
            vulkanTransitionImageLayout(cmd, sourceImage, _swapchainImageLayout,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            _swapchainImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        dst->generateMipmaps(cmd);
        setSceneColorMap(_sceneColorGrabTexture.get());
    }

    void VulkanGraphicsDevice::grabSceneDepth(RenderTarget* source)
    {
        if (!_frameActive || _dynamicRenderingActive) return;
        Texture* sourceTexture = source ? source->depthBuffer() : nullptr;
        auto* src = sourceTexture
            ? dynamic_cast<gpu::VulkanTexture*>(sourceTexture->impl()) : nullptr;
        if (!sourceTexture && _depthImage == VK_NULL_HANDLE) return;
        const uint32_t sourceWidth = sourceTexture
            ? sourceTexture->width() : _swapchainExtent.width;
        const uint32_t sourceHeight = sourceTexture
            ? sourceTexture->height() : _swapchainExtent.height;
        if (!_sceneDepthGrabTexture ||
            _sceneDepthGrabTexture->width() != sourceWidth ||
            _sceneDepthGrabTexture->height() != sourceHeight) {
            TextureOptions options;
            options.name = "sceneDepthGrab";
            options.width = sourceWidth;
            options.height = sourceHeight;
            options.format = sourceTexture
                ? sourceTexture->format() : PixelFormat::PIXELFORMAT_DEPTH;
            options.mipmaps = false;
            _sceneDepthGrabTexture = std::make_shared<Texture>(this, options);
            _sceneDepthGrabTexture->upload();
        }
        auto* dst = dynamic_cast<gpu::VulkanTexture*>(
            _sceneDepthGrabTexture->impl());
        if (!dst) return;
        VkCommandBuffer cmd = _frames[_frameIndex].commandBuffer;
        VkImage sourceImage = src ? src->image() : _depthImage;
        if (src) {
            src->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1, 0, 1);
        } else {
            vulkanTransitionImageLayout(cmd, sourceImage, _depthImageLayout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            _depthImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        dst->transitionLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.extent = {sourceWidth, sourceHeight, 1};
        vkCmdCopyImage(cmd, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst->image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (src) {
            src->transitionLayout(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                0, 1, 0, 1);
        } else {
            vulkanTransitionImageLayout(cmd, sourceImage, _depthImageLayout,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_DEPTH_BIT);
            _depthImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        }
        dst->transitionLayout(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        setSceneDepthGrabMap(_sceneDepthGrabTexture.get());
    }

    void VulkanGraphicsDevice::generateCubemapMips(Texture* cubemap)
    {
        if (!_frameActive || _dynamicRenderingActive || !cubemap) return;
        auto* texture = dynamic_cast<gpu::VulkanTexture*>(cubemap->impl());
        if (texture) {
            texture->generateMipmaps(_frames[_frameIndex].commandBuffer, 0, 6);
        }
    }

    // ─────────────────────────────────────────────────────────────────────
    // Uniform setters
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::setTransformUniforms(
        const Matrix4& viewProjection, const Matrix4& model)
    {
        // Matrix4 is alignas(16) and its union always occupies exactly 64
        // bytes of column-major float matrix data — regardless of whether the
        // build uses SSE / NEON / Apple SIMD / scalar storage.  Copy the whole
        // struct as raw bytes; it lands as 16 floats column-major.
        static_assert(sizeof(Matrix4) == 64, "Matrix4 must be 64 bytes");
        memcpy(_pushConstants.viewProjection, &viewProjection, sizeof(Matrix4));
        memcpy(_pushConstants.model, &model, sizeof(Matrix4));
        _pushConstantsDirty = true;
    }

    void VulkanGraphicsDevice::setDynamicBatchPalette(
        const void* data, const size_t size)
    {
        _pendingPaletteOffset.reset();
        _pendingPaletteSize = 0;
        if (!data || size == 0) {
            return;
        }
        _pendingPaletteOffset = allocateUniform(data, size);
        if (_pendingPaletteOffset) {
            _pendingPaletteSize = size;
        }
    }

    void VulkanGraphicsDevice::setMorphState(
        const std::shared_ptr<VertexBuffer>& deltaBuffer,
        const void* params, const size_t paramsSize)
    {
        _pendingMorphDeltaBuffer.reset();
        _pendingMorphParamsOffset.reset();
        _pendingMorphParamsSize = 0;
        if (!deltaBuffer || !params || paramsSize == 0) {
            return;
        }
        const auto offset = allocateUniform(params, paramsSize);
        if (!offset) {
            return;
        }
        _pendingMorphDeltaBuffer = deltaBuffer;
        _pendingMorphParamsOffset = offset;
        _pendingMorphParamsSize = paramsSize;
    }

    void VulkanGraphicsDevice::setClusterBuffers(
        const void* lightData, const size_t lightSize,
        const void* cellData, const size_t cellSize)
    {
        _clusterLightOffset.reset();
        _clusterCellOffset.reset();
        _clusterLightSize = _clusterCellSize = 0;
        if (!lightData || lightSize == 0 || !cellData || cellSize == 0) return;
        _clusterLightOffset = allocateUniform(lightData, lightSize);
        std::vector<uint32_t> expandedCells(cellSize);
        const auto* bytes = static_cast<const uint8_t*>(cellData);
        for (size_t i = 0; i < cellSize; ++i) expandedCells[i] = bytes[i];
        _clusterCellOffset = allocateUniform(expandedCells.data(),
            expandedCells.size() * sizeof(uint32_t));
        if (_clusterLightOffset && _clusterCellOffset) {
            _clusterLightSize = lightSize;
            _clusterCellSize = expandedCells.size() * sizeof(uint32_t);
        } else {
            _clusterLightOffset.reset();
            _clusterCellOffset.reset();
        }
    }

    void VulkanGraphicsDevice::setClusterGridParams(
        const float* boundsMin, const float* boundsRange,
        const float* cellsCountByBoundsSize, const int cellsX,
        const int cellsY, const int cellsZ, const int maxLightsPerCell,
        const int numClusteredLights)
    {
        for (int i = 0; i < 3; ++i) {
            _lightingUbo.clusterBoundsMin[i] = boundsMin ? boundsMin[i] : 0.0f;
            _lightingUbo.clusterBoundsRange[i] = boundsRange ? boundsRange[i] : 0.0f;
            _lightingUbo.clusterCellsCountByBoundsSize[i] =
                cellsCountByBoundsSize ? cellsCountByBoundsSize[i] : 0.0f;
        }
        _lightingUbo.clusterParams[0] = std::max(cellsX, 0);
        _lightingUbo.clusterParams[1] = std::max(cellsY, 0);
        _lightingUbo.clusterParams[2] = std::max(cellsZ, 0);
        _lightingUbo.clusterParams[3] = std::max(maxLightsPerCell, 0);
        _lightingUbo.clusterParams2[0] = std::max(numClusteredLights, 0);
        _lightingNeedsUpload = true;
    }

    void VulkanGraphicsDevice::setLightingUniforms(const Color& ambientColor,
        const std::vector<GpuLightData>& lights, const Vector3& cameraPosition,
        bool enableNormalMaps, float exposure, const FogParams& fogParams,
        const ShadowParams& shadowParams, int toneMapping,
        const Vector3* ambientSH, const Matrix4* viewProjection)
    {
        if (ambientSH) {
            for (size_t i = 0; i < 9; ++i) {
                _lightingUbo.ambientSH[i][0] = ambientSH[i].getX();
                _lightingUbo.ambientSH[i][1] = ambientSH[i].getY();
                _lightingUbo.ambientSH[i][2] = ambientSH[i].getZ();
                _lightingUbo.ambientSH[i][3] = 0.0f;
            }
        } else {
            std::memset(_lightingUbo.ambientSH, 0,
                sizeof(_lightingUbo.ambientSH));
        }
        (void)viewProjection;

        // Directional cascaded shadows.  The cascade matrices, split distances,
        // and parameters all come straight from the renderer's ShadowParams;
        // the shadow map texture is bound at set 3 in draw().  Only directional
        // (CSM) shadows are wired here — local light shadows come later.
        const bool shadowsOn = shadowParams.enabled && shadowParams.shadowMap != nullptr;
        _shadowMapTexture = shadowsOn ? shadowParams.shadowMap : nullptr;
        if (shadowsOn) {
            std::memcpy(_lightingUbo.shadowMatrices, shadowParams.shadowMatrixPalette,
                sizeof(_lightingUbo.shadowMatrices));
            std::memcpy(_lightingUbo.shadowCascadeDistances, shadowParams.shadowCascadeDistances,
                sizeof(_lightingUbo.shadowCascadeDistances));
        }
        // 0 = off, 1 = PCF depth compare, 2 = EVSM moments (Chebyshev).
        _lightingUbo.shadowParams[0]  = shadowsOn ? (shadowParams.vsm ? 2.0f : 1.0f) : 0.0f;
        _lightingUbo.shadowParams[1]  = static_cast<float>(shadowParams.numCascades);
        _lightingUbo.shadowParams[2]  = shadowParams.bias;
        _lightingUbo.shadowParams[3]  = shadowParams.strength;
        _lightingUbo.shadowParams2[0] = shadowParams.normalBias;
        _lightingUbo.shadowParams2[1] = shadowParams.cascadeBlend;
        _lightingUbo.shadowParams2[2] = static_cast<float>(toneMapping);
        _lightingUbo.shadowParams2[3] = enableNormalMaps ? 1.0f : 0.0f;

        // Local light shadows (spot 2D + omni cubemap), up to 2 casters.  Each
        // light's coneParams[3] carries its slot index (set in the light loop
        // below).  Reset the texture pointers every frame; only active slots
        // rebind them.  Matches MetalUniformBinder::packLocalShadow.
        _localShadowTexture0 = nullptr;
        _localShadowTexture1 = nullptr;
        _omniShadowCube0 = nullptr;
        _omniShadowCube1 = nullptr;
        for (int i = 0; i < ShadowParams::kMaxLocalShadows; ++i) {
            float* matDst    = (i == 0) ? _lightingUbo.localShadowMatrix0 : _lightingUbo.localShadowMatrix1;
            float* paramsDst = (i == 0) ? _lightingUbo.localShadowParams0 : _lightingUbo.localShadowParams1;
            float* omniDst   = (i == 0) ? _lightingUbo.omniShadowParams0  : _lightingUbo.omniShadowParams1;

            if (i >= shadowParams.localShadowCount) {
                std::memset(matDst, 0, 16 * sizeof(float));
                paramsDst[0] = 0.0001f; paramsDst[1] = 0.0f; paramsDst[2] = 1.0f; paramsDst[3] = 0.0f;
                continue;
            }

            const ShadowParams::LocalShadow& ls = shadowParams.localShadows[i];
            if (ls.isOmni) {
                // Omni: bind cubemap, pack [near, far, bias, intensity].  Far is
                // stashed in VP[0][0] by the renderer; the shader bias is a fixed
                // small secondary guard (polygon offset is the primary defence).
                Texture*& cube = (i == 0) ? _omniShadowCube0 : _omniShadowCube1;
                cube = ls.shadowMap;
                omniDst[0] = 0.01f;
                omniDst[1] = ls.viewProjection.getElement(0, 0);
                omniDst[2] = 0.001f;
                omniDst[3] = ls.intensity;
                std::memset(matDst, 0, 16 * sizeof(float));
            } else {
                // Spot: bind 2D depth map, pack the transposed VP matrix in the
                // column-major order the GLSL mat4 expects (mirrors Metal).
                Texture*& tex = (i == 0) ? _localShadowTexture0 : _localShadowTexture1;
                tex = ls.shadowMap;
                for (int col = 0; col < 4; ++col) {
                    for (int row = 0; row < 4; ++row) {
                        matDst[col * 4 + row] = ls.viewProjection.getElement(row, col);
                    }
                }
            }
            paramsDst[0] = ls.bias;
            paramsDst[1] = ls.normalBias;
            paramsDst[2] = ls.intensity;
            paramsDst[3] = ls.isOmni ? 1.0f : 0.0f;
        }

        // Ambient is authored in sRGB; shade in linear space like the Metal path.
        Color ambientLinear;
        ambientLinear.linear(&ambientColor);
        _lightingUbo.ambient[0] = ambientLinear.r;
        _lightingUbo.ambient[1] = ambientLinear.g;
        _lightingUbo.ambient[2] = ambientLinear.b;
        _lightingUbo.ambient[3] = 0.0f;

        _lightingUbo.cameraPosExposure[0] = cameraPosition.getX();
        _lightingUbo.cameraPosExposure[1] = cameraPosition.getY();
        _lightingUbo.cameraPosExposure[2] = cameraPosition.getZ();
        _lightingUbo.cameraPosExposure[3] = exposure;

        constexpr uint32_t kMaxLights = 8;
        const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(lights.size()), kMaxLights);
        _lightingUbo.lightCount[0] = count;

        for (uint32_t i = 0; i < kMaxLights; ++i) {
            VulkanGpuLight& dst = _lightingUbo.lights[i];
            if (i >= count) {
                dst = VulkanGpuLight{};
                dst.colorIntensity[3] = 0.0f;  // zero intensity → contributes nothing
                continue;
            }
            const GpuLightData& src = lights[i];
            Color lightLinear;
            lightLinear.linear(&src.color);

            dst.positionRange[0] = src.position.getX();
            dst.positionRange[1] = src.position.getY();
            dst.positionRange[2] = src.position.getZ();
            dst.positionRange[3] = src.range;

            dst.directionType[0] = src.direction.getX();
            dst.directionType[1] = src.direction.getY();
            dst.directionType[2] = src.direction.getZ();
            dst.directionType[3] =
                static_cast<float>(static_cast<uint32_t>(src.type));

            dst.colorIntensity[0] = lightLinear.r;
            dst.colorIntensity[1] = lightLinear.g;
            dst.colorIntensity[2] = lightLinear.b;
            dst.colorIntensity[3] = src.intensity;

            dst.coneParams[0] = src.innerConeCos;
            dst.coneParams[1] = src.outerConeCos;
            dst.coneParams[2] = src.falloffModeLinear ? 1.0f : 0.0f;
            // Local shadow slot: -1 = no shadow, 0/1 = local caster (indexes
            // localShadowMatrix/Params + the spot/omni depth map bindings).
            dst.coneParams[3] = src.castShadows
                ? static_cast<float>(src.shadowMapIndex)
                : -1.0f;
            dst.areaRightHalfWidth[0] = src.areaRight.getX();
            dst.areaRightHalfWidth[1] = src.areaRight.getY();
            dst.areaRightHalfWidth[2] = src.areaRight.getZ();
            dst.areaRightHalfWidth[3] = src.areaHalfWidth;
            const Vector3 areaUp = src.direction.cross(src.areaRight).normalized();
            dst.areaUpHalfHeight[0] = areaUp.getX();
            dst.areaUpHalfHeight[1] = areaUp.getY();
            dst.areaUpHalfHeight[2] = areaUp.getZ();
            dst.areaUpHalfHeight[3] = src.areaHalfHeight;
        }

        Color fogLinear;
        fogLinear.linear(&fogParams.color);
        _lightingUbo.fogColorDensity[0] = fogLinear.r;
        _lightingUbo.fogColorDensity[1] = fogLinear.g;
        _lightingUbo.fogColorDensity[2] = fogLinear.b;
        _lightingUbo.fogColorDensity[3] = fogParams.density;
        _lightingUbo.fogStartEndType[0] = fogParams.start;
        _lightingUbo.fogStartEndType[1] = fogParams.end;
        _lightingUbo.fogStartEndType[2] = fogParams.enabled ? 1.0f : 0.0f;
        _lightingUbo.fogStartEndType[3] = 0.0f;

        // Re-upload into the ring on the next draw.
        _lightingNeedsUpload = true;
    }

    void VulkanGraphicsDevice::setReflectionProbeUniforms(
        Texture* cubemap, const Vector3& boxMin, const Vector3& boxMax,
        const bool boxProjection, const float intensity, const float maxLod)
    {
        _reflectionProbeTexture = cubemap;
        const Vector3 position = (boxMin + boxMax) * 0.5f;
        const Vector3 values[3] = {boxMin, boxMax, position};
        float* destinations[3] = {
            _lightingUbo.reflectionProbeBoxMin,
            _lightingUbo.reflectionProbeBoxMax,
            _lightingUbo.reflectionProbePosition};
        for (int v = 0; v < 3; ++v) {
            destinations[v][0] = values[v].getX();
            destinations[v][1] = values[v].getY();
            destinations[v][2] = values[v].getZ();
        }
        _lightingUbo.reflectionProbeParams[0] = boxProjection ? 1.0f : 0.0f;
        _lightingUbo.reflectionProbeParams[1] = intensity;
        _lightingUbo.reflectionProbeParams[2] = maxLod;
        _lightingNeedsUpload = true;
    }

    void VulkanGraphicsDevice::setEnvironmentUniforms(
        Texture* envAtlas, float skyboxIntensity, float skyboxMip,
        const Vector3& skyDomeCenter, bool isDome, Texture* skyboxCubeMap)
    {
        _envAtlasTexture = envAtlas;
        _skyboxCubeTexture = skyboxCubeMap;

        // skyParams2: xyz = dome center, w = flags (bit0 cubemap, bit1 dome).
        _lightingUbo.skyParams2[0] = skyDomeCenter.getX();
        _lightingUbo.skyParams2[1] = skyDomeCenter.getY();
        _lightingUbo.skyParams2[2] = skyDomeCenter.getZ();
        _lightingUbo.skyParams2[3] = static_cast<float>(
            (skyboxCubeMap ? 1u : 0u) | (isDome ? 2u : 0u));

        _lightingUbo.envParams[0] = skyboxIntensity;
        _lightingUbo.envParams[1] = envAtlas ? 1.0f : 0.0f;
        if (envAtlas) {
            switch (envAtlas->encoding()) {
            case TextureEncoding::RGBP:
                _lightingUbo.envParams[2] = static_cast<float>(VulkanEnvEncoding::Rgbp);
                break;
            case TextureEncoding::RGBM:
                _lightingUbo.envParams[2] = static_cast<float>(VulkanEnvEncoding::Rgbm);
                break;
            default:
                _lightingUbo.envParams[2] = static_cast<float>(VulkanEnvEncoding::Srgb);
                break;
            }
        }
        _lightingUbo.envParams[3] = skyboxMip;

        _lightingNeedsUpload = true;
    }

    // ─────────────────────────────────────────────────────────────────────
    // Resource creation
    // ─────────────────────────────────────────────────────────────────────

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

    // ─────────────────────────────────────────────────────────────────────
    // Display management
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::setResolution(int width, int height)
    {
        if (width == _width && height == _height &&
            !_swapchainRecreationPending) {
            return;
        }
        _width = width;
        _height = height;
        _swapchainRecreationPending = true;
        (void)recreateSwapchain();
    }

    void VulkanGraphicsDevice::recoverFailedFrameSubmission(
        const VkResult result)
    {
        spdlog::error(
            "VulkanGraphicsDevice: frame submission failed ({}); "
            "recovering frame synchronization",
            static_cast<int>(result));

        if (result == VK_ERROR_DEVICE_LOST) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: device lost; rendering disabled");
            return;
        }

        auto& frame = _frames[_frameIndex];

        // The failed submission did not consume imageAvailable. Reusing that
        // binary semaphore in vkAcquireNextImageKHR would try to signal an
        // already-signaled semaphore. Consume it with an empty queue submission
        // and use the frame fence so swapchain recreation can wait for it.
        const VkResult resetResult =
            vkResetFences(_device, 1, &frame.inFlightFence);
        if (resetResult != VK_SUCCESS) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: failed to reset recovery fence ({}); "
                "rendering disabled",
                static_cast<int>(resetResult));
            return;
        }
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkSubmitInfo consumeAcquire{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        consumeAcquire.waitSemaphoreCount = 1;
        consumeAcquire.pWaitSemaphores = &frame.imageAvailable;
        consumeAcquire.pWaitDstStageMask = &waitStage;
        const VkResult recoverySubmit = vkQueueSubmit(
            _graphicsQueue, 1, &consumeAcquire, frame.inFlightFence);
        if (recoverySubmit != VK_SUCCESS) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: synchronization recovery submit failed "
                "({}); rendering disabled",
                static_cast<int>(recoverySubmit));
            return;
        }

        // The failed frame acquired an image but never presented it. Rebuilding
        // releases that image and gives the next frame a clean WSI state.
        if (!recreateSwapchain()) {
            _renderingDisabled = true;
        }
    }

    bool VulkanGraphicsDevice::recreateSwapchain()
    {
        if (_device == VK_NULL_HANDLE) {
            return false;
        }

        int drawableWidth = 0;
        int drawableHeight = 0;
        const bool drawableSizeAvailable =
            _window != nullptr &&
            SDL_GetWindowSizeInPixels(
                _window, &drawableWidth, &drawableHeight);
        if (_width <= 0 || _height <= 0 ||
            (drawableSizeAvailable &&
             (drawableWidth <= 0 || drawableHeight <= 0))) {
            _swapchainRecreationPending = true;
            return true;
        }

        RetiredSwapchain retired{};
        retired.frame = _frameNumber;
        retired.swapchain = _swapchain;
        retired.imageViews = std::move(_swapchainImageViews);
        retired.depthImage = _depthImage;
        retired.depthAllocation = _depthAllocation;
        retired.depthImageView = _depthImageView;
        retired.renderFinishedSemaphores =
            std::move(_renderFinishedSemaphores);
        const VkImageLayout oldDepthLayout = _depthImageLayout;

        _swapchainImages.clear();
        _depthImage = VK_NULL_HANDLE;
        _depthAllocation = VK_NULL_HANDLE;
        _depthImageView = VK_NULL_HANDLE;
        _depthImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (!initSwapchain(
                _width, _height, retired.swapchain)) {
            // vkCreateSwapchainKHR failure leaves oldSwapchain usable. A
            // later setup failure does not, and initSwapchain marks that by
            // clearing _swapchain after destroying the unusable replacement.
            if (_swapchain == retired.swapchain) {
                _swapchainImageViews =
                    std::move(retired.imageViews);
                _depthImage = retired.depthImage;
                _depthAllocation = retired.depthAllocation;
                _depthImageView = retired.depthImageView;
                _depthImageLayout = oldDepthLayout;
                _renderFinishedSemaphores =
                    std::move(retired.renderFinishedSemaphores);
            } else {
                _retiredSwapchains.push_back(std::move(retired));
            }
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: swapchain recreation failed; "
                "rendering disabled");
            return false;
        }

        _retiredSwapchains.push_back(std::move(retired));

        try {
            createDepthResources();
        } catch (const std::exception& error) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: swapchain depth recreation failed: {}; "
                "rendering disabled",
                error.what());
            return false;
        }
        // Image count may differ in the new swapchain — per-image
        // semaphores must match it.
        if (!createSwapchainSemaphores()) {
            _renderingDisabled = true;
            spdlog::error(
                "VulkanGraphicsDevice: presentation semaphore recreation "
                "failed; rendering disabled");
            return false;
        }
        _swapchainRecreationPending = false;
        return true;
    }

    std::pair<int, int> VulkanGraphicsDevice::size() const
    {
        return {_width, _height};
    }
}

#endif // VISUTWIN_HAS_VULKAN
