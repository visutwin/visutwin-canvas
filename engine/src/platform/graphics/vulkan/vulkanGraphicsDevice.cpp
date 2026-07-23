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
#include "platform/graphics/renderPass.h"
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
    // ─────────────────────────────────────────────────────────────────────
    // Construction / Destruction
    // ─────────────────────────────────────────────────────────────────────

    VulkanGraphicsDevice::VulkanGraphicsDevice(const GraphicsDeviceOptions& options)
    {
        _window = options.window;
        _validationEnabled = options.enableValidation;

        int w = 0, h = 0;
        SDL_GetWindowSize(_window, &w, &h);
        _width = w;
        _height = h;

        initInstance(_window);
        initDevice();

        // VMA allocator
        VmaAllocatorCreateInfo allocatorInfo{};
        allocatorInfo.physicalDevice = _physicalDevice;
        allocatorInfo.device = _device;
        allocatorInfo.instance = _instance;
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
        vmaCreateAllocator(&allocatorInfo, &_vmaAllocator);

        // Fail loudly instead of limping on: every later frame call would
        // dereference these (e.g. _swapchainImages[_swapchainImageIndex]).
        if (_instance == VK_NULL_HANDLE || _device == VK_NULL_HANDLE ||
            _surface == VK_NULL_HANDLE || _vmaAllocator == VK_NULL_HANDLE) {
            throw std::runtime_error("VulkanGraphicsDevice: instance/device/surface initialization failed");
        }

        initSwapchain(_width, _height);
        createDepthResources();
        createPerFrameResources();

        if (_swapchain == VK_NULL_HANDLE || _swapchainImages.empty()) {
            throw std::runtime_error("VulkanGraphicsDevice: swapchain creation failed");
        }

        // Upload command pool (batched, nonblocking staging transfers)
        VkCommandPoolCreateInfo uploadPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        uploadPoolInfo.queueFamilyIndex = _graphicsQueueFamily;
        vkCreateCommandPool(_device, &uploadPoolInfo, nullptr, &_uploadCommandPool);

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
        vkCreateSampler(_device, &samplerInfo, nullptr, &_defaultSampler);

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
        vkCreateSampler(_device, &envSamplerInfo, nullptr, &_envSampler);

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
        vkCreateSampler(_device, &shadowSamplerInfo, nullptr, &_shadowSampler);

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
            vmaCreateImage(_vmaAllocator, &imgInfo, &aInfo, &_whiteImage, &_whiteAllocation, nullptr);

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = _whiteImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(_device, &viewInfo, nullptr, &_whiteImageView);

            // Upload single white pixel
            uint32_t whitePixel = 0xFFFFFFFF;
            VkBuffer stagingBuf;
            VmaAllocation stagingAlloc;
            VkBufferCreateInfo sInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            sInfo.size = 4;
            sInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo saInfo{};
            saInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            vmaCreateBuffer(_vmaAllocator, &sInfo, &saInfo, &stagingBuf, &stagingAlloc, nullptr);
            void* mapped;
            vmaMapMemory(_vmaAllocator, stagingAlloc, &mapped);
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
            vmaCreateImage(_vmaAllocator, &imgInfo, &aInfo, &_whiteCubeImage, &_whiteCubeAllocation, nullptr);

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = _whiteCubeImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
            viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
            vkCreateImageView(_device, &viewInfo, nullptr, &_whiteCubeImageView);

            uint32_t whitePixels[6] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                       0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
            VkBuffer stagingBuf;
            VmaAllocation stagingAlloc;
            VkBufferCreateInfo sInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            sInfo.size = sizeof(whitePixels);
            sInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo saInfo{};
            saInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            vmaCreateBuffer(_vmaAllocator, &sInfo, &saInfo, &stagingBuf, &stagingAlloc, nullptr);
            void* mapped;
            vmaMapMemory(_vmaAllocator, stagingAlloc, &mapped);
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
        flushDeferredDestroys(true);

        destroyPostResources();

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
        _uboOffsetAlignment = props.limits.minUniformBufferOffsetAlignment;
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

        auto qr = vkbDevice.get_queue(vkb::QueueType::graphics);
        if (qr) _graphicsQueue = qr.value();
        auto qi = vkbDevice.get_queue_index(vkb::QueueType::graphics);
        if (qi) _graphicsQueueFamily = qi.value();
    }

    void VulkanGraphicsDevice::initSwapchain(int width, int height)
    {
        // Use a linear (UNORM) swapchain — the shaders apply manual
        // pow(1/2.2) for display gamma encoding, matching the Metal path
        // which renders into a non-sRGB BGRA8Unorm drawable.  Choosing
        // VK_FORMAT_B8G8R8A8_SRGB instead would make the hardware apply a
        // second sRGB encode on store, doubling the gamma and washing out
        // the rendered scene.
        vkb::SwapchainBuilder swapBuilder{_physicalDevice, _device, _surface};
        swapBuilder.set_desired_extent(static_cast<uint32_t>(width), static_cast<uint32_t>(height))
                   .set_desired_format({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
                   .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
                   .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        auto result = swapBuilder.build();
        if (!result) {
            spdlog::error("Failed to create Vulkan swapchain: {}", result.error().message());
            return;
        }
        auto vkbSwap = result.value();
        _swapchain = vkbSwap.swapchain;
        _swapchainFormat = vkbSwap.image_format;
        _swapchainExtent = vkbSwap.extent;
        _swapchainImages = vkbSwap.get_images().value();
        _swapchainImageViews = vkbSwap.get_image_views().value();

        // Adopt the actual swapchain extent as the device size.  The requested
        // width/height can be stale or zero — SDL_GetWindowSize may report 0×0
        // before the window is first shown — but the surface clamps the
        // swapchain to its real size.  size() (and therefore the renderer's
        // viewport/scissor) must reflect that, or every draw collapses to a
        // 1×1 viewport.
        _width = static_cast<int>(_swapchainExtent.width);
        _height = static_cast<int>(_swapchainExtent.height);
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
            vkCreateCommandPool(_device, &poolInfo, nullptr, &frame.commandPool);

            VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = frame.commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vkAllocateCommandBuffers(_device, &allocInfo, &frame.commandBuffer);

            VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(_device, &semInfo, nullptr, &frame.imageAvailable);

            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(_device, &fenceInfo, nullptr, &frame.inFlightFence);

            const VkDescriptorPool descriptorPool =
                createFrameDescriptorPool(kInitialDescriptorSets);
            if (descriptorPool == VK_NULL_HANDLE) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: per-frame descriptor pool creation failed");
            }
            frame.descriptorPools.push_back(
                {descriptorPool, kInitialDescriptorSets});
        }

        createSwapchainSemaphores();
    }

    VkDescriptorPool VulkanGraphicsDevice::createFrameDescriptorPool(
        const uint32_t maxSets)
    {
        // Every cached image set has at most seven combined samplers. Post
        // passes additionally consume one ordinary UBO descriptor per set.
        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0] = {
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            maxSets * kMaxCachedImageBindings
        };
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxSets};

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
        for (uint32_t i = 0; i < key.count; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
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

    void VulkanGraphicsDevice::createSwapchainSemaphores()
    {
        // One renderFinished semaphore per swapchain image — recreated
        // alongside the swapchain because the image count can change on
        // resize.
        _renderFinishedSemaphores.resize(_swapchainImages.size(), VK_NULL_HANDLE);
        VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : _renderFinishedSemaphores) {
            vkCreateSemaphore(_device, &semInfo, nullptr, &s);
        }
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
        auto& frame = _frames[_frameIndex];

        vkWaitForFences(_device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        collectUploads(false);
        flushUploads();

        // The fence wait proves frame (_frameNumber - kMaxFramesInFlight) has
        // completed on the GPU — release resources queued up to that frame.
        flushDeferredDestroys(false);

        VkResult result = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
            frame.imageAvailable, VK_NULL_HANDLE, &_swapchainImageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            // Recreate directly — setResolution(_width, _height) would
            // early-return on the unchanged size and never rebuild the
            // swapchain, wedging every subsequent frame.
            recreateSwapchain();
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

        // Reset the fence only once the frame is guaranteed to submit —
        // resetting before a skipped frame would deadlock the next wait.
        vkResetFences(_device, 1, &frame.inFlightFence);
        _frameActive = true;

        // A fresh swapchain image is always in an undefined layout — vkAcquire
        // doesn't promise any particular contents.  Track this so onFrameEnd
        // can pick the right source layout for the PRESENT transition; and so
        // startRenderPass can use UNDEFINED as the discard source when first
        // attaching the image (which is fine because it gets LOAD_OP_CLEAR /
        // LOAD_OP_DONT_CARE on the colour attachment).
        _swapchainImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        vkResetCommandBuffer(frame.commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);

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

        vkEndCommandBuffer(cmd);

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
        vkQueueSubmit(_graphicsQueue, 1, &submitInfo, frame.inFlightFence);

        // Present
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &renderFinished;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &_swapchain;
        presentInfo.pImageIndices = &_swapchainImageIndex;
        const VkResult presentResult = vkQueuePresentKHR(_graphicsQueue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
            // Present is the usual place a resize surfaces — rebuild now so the
            // next acquire starts from a valid swapchain.
            recreateSwapchain();
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
        if (_vsmBlurPipelineLayout != VK_NULL_HANDLE) {
            return;
        }

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.bindingCount = 1;
        setInfo.pBindings = &binding;
        vkCreateDescriptorSetLayout(_device, &setInfo, nullptr, &_vsmBlurSetLayout);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = 32; // vec4 dirInvRes + vec4 filterParams

        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &_vsmBlurSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(_device, &layoutInfo, nullptr, &_vsmBlurPipelineLayout);

        auto createModule = [this](const uint32_t* spirv, size_t words) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = words * sizeof(uint32_t);
            info.pCode = spirv;
            VkShaderModule module = VK_NULL_HANDLE;
            vkCreateShaderModule(_device, &info, nullptr, &module);
            return module;
        };
        _vsmBlurVertModule = createModule(
            vulkan_generated::kVsmBlurVert,
            vulkan_generated::kVsmBlurVertWordCount);
        _vsmBlurFragModule = createModule(
            vulkan_generated::kVsmBlurFrag,
            vulkan_generated::kVsmBlurFragWordCount);
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
        auto colorOps = renderPass ? renderPass->colorOps() : nullptr;
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

            for (const auto& att : offscreen->colorAttachments()) {
                if (!att.texture) continue;
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
        (void)renderPass;
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
            const auto colorOps = renderPass ? renderPass->colorOps() : nullptr;
            for (const auto& att : _activeOffscreenTarget->colorAttachments()) {
                if (!att.texture) continue;
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

    // ─────────────────────────────────────────────────────────────────────
    // Core rendering
    // ─────────────────────────────────────────────────────────────────────

    void VulkanGraphicsDevice::draw(const Primitive& primitive,
        const std::shared_ptr<IndexBuffer>& indexBuffer,
        int numInstances, int indirectSlot, bool first, bool last)
    {
        (void)indirectSlot;
        if (!_shader || !_dynamicRenderingActive) return;

        auto& frame = _frames[_frameIndex];
        VkCommandBuffer cmd = frame.commandBuffer;

        auto vulkanShader = std::dynamic_pointer_cast<VulkanShader>(_shader);
        if (!vulkanShader || vulkanShader->vertexModule() == VK_NULL_HANDLE) return;

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
            VkFormat colorFmt = _swapchainFormat;
            VkFormat depthFmt = _depthFormat;
            if (_activeOffscreenTarget) {
                const auto& colors = _activeOffscreenTarget->colorAttachments();
                colorFmt = colors.empty() ? VK_FORMAT_UNDEFINED : colors[0].format;
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
                colorFmt, depthFmt, isSkybox);

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
            std::array<VkDescriptorImageInfo, 6> imageInfos{};
            for (auto& imageInfo : imageInfos) {
                imageInfo.sampler = _defaultSampler;
                imageInfo.imageView = _whiteImageView;
                imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            if (_material) {
                std::vector<TextureSlot> texSlots;
                _material->getTextureSlots(texSlots);
                for (const auto& ts : texSlots) {
                    if (ts.slot < 0 || ts.slot >= 6 || ts.texture == nullptr) {
                        continue;
                    }
                    auto* vkTex =
                        static_cast<gpu::VulkanTexture*>(ts.texture->impl());
                    if (vkTex && vkTex->imageView() != VK_NULL_HANDLE) {
                        imageInfos[ts.slot].imageView = vkTex->imageView();
                        if (vkTex->sampler() != VK_NULL_HANDLE) {
                            imageInfos[ts.slot].sampler = vkTex->sampler();
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

            std::array<VkDescriptorImageInfo, 7> sceneInfos{};
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

        // Draw
        if (indexBuffer) {
            auto* ib = static_cast<VulkanIndexBuffer*>(indexBuffer.get());
            if (ib->buffer() != VK_NULL_HANDLE) {
                VkIndexType idxType = (indexBuffer->format() == INDEXFORMAT_UINT32)
                    ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
                vkCmdBindIndexBuffer(cmd, ib->buffer(), 0, idxType);
                vkCmdDrawIndexed(cmd, primitive.count, numInstances,
                    primitive.base, primitive.baseVertex, 0);
            }
        } else {
            vkCmdDraw(cmd, primitive.count, numInstances, primitive.base, 0);
        }

        recordDrawCall();

        if (last) {
            clearVertexBuffer();
            _currentPipeline = VK_NULL_HANDLE;
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

    void VulkanGraphicsDevice::setLightingUniforms(const Color& ambientColor,
        const std::vector<GpuLightData>& lights, const Vector3& cameraPosition,
        bool enableNormalMaps, float exposure, const FogParams& fogParams,
        const ShadowParams& shadowParams, int toneMapping,
        const Vector3* ambientSH, const Matrix4* viewProjection)
    {
        // The baseline Vulkan lighting shader does not consume ambient SH or
        // the auxiliary view-projection matrix yet. Keep the interface in sync
        // with GraphicsDevice while those feature bindings are ported.
        (void)ambientSH;
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
            // AreaRect (3) has no dedicated path yet — fall back to point shading.
            dst.directionType[3] = (src.type == GpuLightType::AreaRect)
                ? static_cast<float>(VulkanLightTypeTag::Point)
                : static_cast<float>(static_cast<uint32_t>(src.type));

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
        if (width == _width && height == _height) return;
        _width = width;
        _height = height;
        recreateSwapchain();
    }

    void VulkanGraphicsDevice::recreateSwapchain()
    {
        if (_device == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(_device);
        flushDeferredDestroys(true); // idle device — everything is safe to free
        destroyDepthResources();
        destroySwapchainSemaphores();
        cleanupSwapchain();
        initSwapchain(_width, _height);
        createDepthResources();
        // Image count may differ in the new swapchain — per-image
        // semaphores must match it.
        createSwapchainSemaphores();
    }

    std::pair<int, int> VulkanGraphicsDevice::size() const
    {
        return {_width, _height};
    }
}

#endif // VISUTWIN_HAS_VULKAN
