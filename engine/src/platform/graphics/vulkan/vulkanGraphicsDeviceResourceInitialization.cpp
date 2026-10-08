// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 27.07.2026
//

#ifdef VISUTWIN_HAS_VULKAN

#define VMA_IMPLEMENTATION
#include "vulkanGraphicsDevice.h"

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
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

#include "core/scopedTimer.h"
#include "core/math/color.h"
#include "core/math/vector3.h"
#include "platform/graphics/compute.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/shaderFeatures.h"
#include "platform/graphics/texture.h"
#include "scene/materials/material.h"
#include "spdlog/spdlog.h"


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

    namespace
    {
        struct SamplerDesc
        {
            VkFilter filter = VK_FILTER_LINEAR;
            VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            float maxLod = VK_LOD_CLAMP_NONE;
            VkBool32 anisotropyEnable = VK_FALSE;
            float maxAnisotropy = 0.0f;
        };

        /// One of the device's shared samplers; throws, naming it, when it cannot be made.
        VkSampler createDeviceSampler(VkDevice device, const SamplerDesc& desc, const char* name)
        {
            VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            info.magFilter = desc.filter;
            info.minFilter = desc.filter;
            info.mipmapMode = desc.mipmapMode;
            info.addressModeU = desc.addressMode;
            info.addressModeV = desc.addressMode;
            info.addressModeW = desc.addressMode;
            info.maxLod = desc.maxLod;
            info.anisotropyEnable = desc.anisotropyEnable;
            info.maxAnisotropy = desc.maxAnisotropy;
            VkSampler sampler = VK_NULL_HANDLE;
            if (vkCreateSampler(device, &info, nullptr, &sampler) != VK_SUCCESS) {
                throw std::runtime_error(
                    std::string("VulkanGraphicsDevice: ") + name + " sampler creation failed");
            }
            return sampler;
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

        createAllocator();
        createShaderCaches(options);
        spdlog::info("Vulkan back buffer: {}x MSAA", resolveBackBufferSamples(options.antialias));

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

        createUploadCommandPool();
        _renderPipeline = std::make_unique<VulkanRenderPipeline>(this);
        createSamplers();
        createFallbackImages();
        createUniformRing();

        // GPU pass profiler. Disabled by default (sampling costs a little), so
        // creating it here is cheap — it only allocates its query pools.
        // Null when the queue family reports no valid timestamp bits, which
        // leaves the base gpuProfiler() accessor returning null exactly as it
        // does on a backend without profiling.
        _vulkanGpuProfiler = gpu::VulkanGpuProfiler::create(
            _device, _physicalDevice, _graphicsQueueFamily);
        _gpuProfiler = _vulkanGpuProfiler;

        spdlog::info("VulkanGraphicsDevice initialized ({}x{})", _width, _height);
    }

    void VulkanGraphicsDevice::createAllocator()
    {
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
    }

    void VulkanGraphicsDevice::createUploadCommandPool()
    {
        // Batched, nonblocking staging transfers record into buffers from this pool.
        VkCommandPoolCreateInfo uploadPoolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        uploadPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        uploadPoolInfo.queueFamilyIndex = _graphicsQueueFamily;
        if (vkCreateCommandPool(
                _device, &uploadPoolInfo, nullptr,
                &_uploadCommandPool) != VK_SUCCESS) {
            throw std::runtime_error(
                "VulkanGraphicsDevice: upload command pool creation failed");
        }
    }

    void VulkanGraphicsDevice::createSamplers()
    {
        // Every sampler created here must also be released by destroySamplers().

        // Default: linear, repeat, full mip chain.
        _defaultSampler = createDeviceSampler(_device, SamplerDesc{}, "default");

        // Environment-atlas sampler: clamp-to-edge so the equirectangular seam
        // and the packed sub-rects (irradiance, roughness mips) never wrap
        // into each other.  Trilinear; no anisotropy (matches the Metal
        // envAtlasSampler rationale — anisotropy smears the atan2 wrap).
        _envSampler = createDeviceSampler(_device,
            SamplerDesc{.addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE}, "environment");

        // Shared sampler for the separate material images (height, detail
        // normal, displacement and the three clearcoat maps). Linear + repeat
        // AND the device's anisotropy ratio, exactly like the per-texture
        // samplers in vulkanTexture.cpp and Metal's default sampler: on Metal
        // these maps are read through that default sampler, so a separate image
        // filtered without anisotropy is a backend divergence on every oblique
        // surface. Invisible on the smooth parallax height map, obvious on a
        // ribbed clearcoat normal map. maxAnisotropy() is published by
        // initDevice(), which has run by now.
        _materialExtraSampler = createDeviceSampler(_device, SamplerDesc{
            .anisotropyEnable = maxAnisotropy() > 1.0f ? VK_TRUE : VK_FALSE,
            .maxAnisotropy = std::max(maxAnisotropy(), 1.0f),
        }, "material");

        // Shadow-map sampler: clamp-to-edge, NEAREST filter, no mips.  A plain
        // (non-comparison) sampler — the shader does the depth compare manually
        // and averages a 3×3 PCF kernel at discrete texel offsets, so no linear
        // filtering is needed (and depth formats like D32_SFLOAT often don't
        // support VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR anyway).
        _shadowSampler = createDeviceSampler(_device, SamplerDesc{
            .filter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 0.0f,
        }, "shadow");
    }

    void VulkanGraphicsDevice::createFallbackImages()
    {
        // Every image created here must also be released by destroyFallbackImages().

        // 1×1 white texture: the fallback for unbound texture slots.
        createWhiteImage(1, _whiteImage, _whiteAllocation, _whiteImageView, "image");

        // 1×1 white cubemap: the fallback for unbound omni shadow slots. Six
        // layers + cube-compatible so it can back a samplerCube descriptor;
        // every face is white so an unshadowed omni light reads fully lit.
        createWhiteImage(6, _whiteCubeImage, _whiteCubeAllocation, _whiteCubeImageView, "cubemap");
    }

    void VulkanGraphicsDevice::createWhiteImage(const uint32_t layers, VkImage& image,
        VmaAllocation& allocation, VkImageView& view, const char* name)
    {
        const bool cube = layers == 6;
        const auto fail = [name](const char* what) {
            return std::runtime_error(
                std::string("VulkanGraphicsDevice: fallback ") + name + " " + what);
        };

        VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {1, 1, 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = layers;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;

        VmaAllocationCreateInfo aInfo{};
        aInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(_vmaAllocator, &imgInfo, &aInfo, &image, &allocation, nullptr) != VK_SUCCESS) {
            throw fail("creation failed");
        }

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        if (vkCreateImageView(_device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
            throw fail("view creation failed");
        }

        // One white RGBA8 texel per layer.
        const std::vector<uint32_t> whitePixels(layers, 0xFFFFFFFFu);
        const VkDeviceSize stagingSize = whitePixels.size() * sizeof(uint32_t);
        VkBuffer stagingBuf = VK_NULL_HANDLE;
        VmaAllocation stagingAlloc = VK_NULL_HANDLE;
        VkBufferCreateInfo sInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sInfo.size = stagingSize;
        sInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo saInfo{};
        saInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
        if (vmaCreateBuffer(_vmaAllocator, &sInfo, &saInfo, &stagingBuf, &stagingAlloc, nullptr) != VK_SUCCESS) {
            throw fail("staging buffer creation failed");
        }
        void* mapped = nullptr;
        if (vmaMapMemory(_vmaAllocator, stagingAlloc, &mapped) != VK_SUCCESS) {
            vmaDestroyBuffer(_vmaAllocator, stagingBuf, stagingAlloc);
            throw fail("staging buffer mapping failed");
        }
        memcpy(mapped, whitePixels.data(), stagingSize);
        vmaUnmapMemory(_vmaAllocator, stagingAlloc);

        enqueueUpload([image, stagingBuf, layers](VkCommandBuffer cmd) {
            vulkanTransitionImageLayout(cmd, image,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
            // One copy per layer (a cubemap's faces are distinct array layers).
            std::vector<VkBufferImageCopy> regions(layers);
            for (uint32_t layer = 0; layer < layers; ++layer) {
                regions[layer].bufferOffset = layer * sizeof(uint32_t);
                regions[layer].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
                regions[layer].imageExtent = {1, 1, 1};
            }
            vkCmdCopyBufferToImage(cmd, stagingBuf, image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers, regions.data());
            vulkanTransitionImageLayout(cmd, image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
        }, [allocator = _vmaAllocator, stagingBuf, stagingAlloc] {
            vmaDestroyBuffer(allocator, stagingBuf, stagingAlloc);
        });
    }

    void VulkanGraphicsDevice::createUniformRing()
    {
        // Per-draw / per-pass uniform ring buffer.  Sized for a generous draw
        // count per frame: each region holds material + lighting slots for the
        // whole frame.  Slot sizes are aligned up to the device's dynamic-UBO
        // offset granularity inside the ring allocator.
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

        // Allocation only: the buffer the set NAMES is written by
        // writeUniformRingDescriptors below, which also runs after the ring
        // grows — so the two paths cannot describe the buffer differently.
        auto allocSet = [&](VkDescriptorSetLayout layout) {
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = _persistentDescriptorPool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &layout;
            if (vkAllocateDescriptorSets(_device, &ai, &set) != VK_SUCCESS) {
                throw std::runtime_error(
                    "VulkanGraphicsDevice: persistent descriptor allocation failed");
            }
            return set;
        };

        // Range covers the largest block bound here, not just MaterialUniforms —
        // quad effects share this slot and some carry more (volumetric fog: 512 B).
        static_assert(sizeof(MaterialUniforms) <= kPerDrawUniformCapacity);
        _materialDescriptorSet = allocSet(_renderPipeline->materialSetLayout());
        _lightingDescriptorSet = allocSet(_renderPipeline->lightingSetLayout());
        writeUniformRingDescriptors();
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

    void VulkanGraphicsDevice::destroyFallbackImages() noexcept
    {
        // Same contract as destroySamplers(): the ONLY place the constructor's
        // fallback images are released, so both teardown paths stay in step:
        // separate hand-written lists drift, and a resource missing from one leaks.
        for (auto& [view, image, allocation] :
                {std::tie(_whiteImageView, _whiteImage, _whiteAllocation),
                 std::tie(_whiteCubeImageView, _whiteCubeImage, _whiteCubeAllocation)}) {
            if (_device != VK_NULL_HANDLE && view != VK_NULL_HANDLE) {
                vkDestroyImageView(_device, view, nullptr);
                view = VK_NULL_HANDLE;
            }
            if (_vmaAllocator != VK_NULL_HANDLE && image != VK_NULL_HANDLE) {
                vmaDestroyImage(_vmaAllocator, image, allocation);
                image = VK_NULL_HANDLE;
                allocation = VK_NULL_HANDLE;
            }
        }
    }

    void VulkanGraphicsDevice::createShaderCaches(const GraphicsDeviceOptions& options)
    {
        _shaderDiskCache = ShaderDiskCache(ShaderDiskCache::resolveDirectory(
            options.shaderCacheDirectory, options.persistentShaderCache));
        if (_shaderDiskCache.enabled()) {
            setVulkanShaderDiskCache(&_shaderDiskCache);
        }

        // The pipeline cache belongs to one device and one driver build; the driver
        // checks its own header as well, and starts empty if it does not like the data.
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_physicalDevice, &properties);
        _pipelineCacheKey = "vulkan pipeline cache\n";
        _pipelineCacheKey += "vendor " + std::to_string(properties.vendorID) +
            " device " + std::to_string(properties.deviceID) +
            " driver " + std::to_string(properties.driverVersion) + "\nuuid";
        for (const uint8_t byte : properties.pipelineCacheUUID) {
            _pipelineCacheKey += " " + std::to_string(byte);
        }

        const auto stored = _shaderDiskCache.load("pipelines", _pipelineCacheKey);
        VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (stored) {
            info.initialDataSize = stored->size();
            info.pInitialData = stored->data();
        }
        if (vkCreatePipelineCache(_device, &info, nullptr, &_pipelineCache) != VK_SUCCESS) {
            // Data the driver refuses outright: try once more with none.
            info.initialDataSize = 0;
            info.pInitialData = nullptr;
            if (vkCreatePipelineCache(_device, &info, nullptr, &_pipelineCache) != VK_SUCCESS) {
                _pipelineCache = VK_NULL_HANDLE;
            }
        }
        if (_shaderDiskCache.enabled()) {
            spdlog::info("Vulkan shader cache: '{}' ({} bytes of pipeline cache loaded)",
                _shaderDiskCache.directory().string(), stored ? stored->size() : 0);
        }
    }

    void VulkanGraphicsDevice::savePipelineCache()
    {
        _pipelinesSaved = _pipelinesCreated;
        if (_pipelineCache == VK_NULL_HANDLE || !_shaderDiskCache.enabled()) {
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        size_t size = 0;
        if (vkGetPipelineCacheData(_device, _pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) {
            return;
        }
        std::vector<uint8_t> data(size);
        const VkResult result = vkGetPipelineCacheData(_device, _pipelineCache, &size, data.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            return;
        }
        data.resize(size);
        if (_shaderDiskCache.store("pipelines", _pipelineCacheKey, data)) {
            spdlog::debug("Vulkan shader cache: wrote {} bytes of pipeline cache in {:.1f} ms", data.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
    }

    void VulkanGraphicsDevice::savePipelineCacheWhenSettled()
    {
        // Once per burst of new pipelines, two seconds or so after the last of them:
        // serializing the cache is not free, and a scene that is still loading would
        // write it again and again.
        constexpr int kSettledFrames = 120;
        if (_pipelinesCreated != _pipelinesSaved && ++_framesSincePipelineCreated > kSettledFrames) {
            savePipelineCache();
        }
    }

    void VulkanGraphicsDevice::destroyShaderCaches()
    {
        if (_pipelinesCreated != _pipelinesSaved) {
            savePipelineCache();
        }
        if (_pipelineCache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(_device, _pipelineCache, nullptr);
            _pipelineCache = VK_NULL_HANDLE;
        }
        releaseVulkanShaderDiskCache(&_shaderDiskCache);
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
        collectRetiredSwapchains(true);

        destroyComputeResources();


        _renderPipeline.reset();
        // After the last pipeline is gone; written back first if this run added to it.
        destroyShaderCaches();
        // Owns VkQueryPools — must die before the VkDevice.
        _vulkanGpuProfiler.reset();
        _gpuProfiler.reset();

        if (_persistentDescriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(_device, _persistentDescriptorPool, nullptr);
        _uniformRing.reset();

        destroySamplers();
        destroyFallbackImages();

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
        // The pipeline cache and the shader disk cache are made early in initialization,
        // so an initialization that fails later owns them too; the destructor's path is
        // not taken here.
        if (_device != VK_NULL_HANDLE) {
            destroyShaderCaches();
        }
        _vulkanGpuProfiler.reset();
        _gpuProfiler.reset();

        if (_device != VK_NULL_HANDLE) {
            if (_persistentDescriptorPool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(
                    _device, _persistentDescriptorPool, nullptr);
                _persistentDescriptorPool = VK_NULL_HANDLE;
            }
            destroySamplers();
        }
        // Handles both the views (needs _device) and the images (needs the VMA
        // allocator); either being null just skips its half.
        destroyFallbackImages();
        _uniformRing.reset();

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

        auto physResult = selector.select_devices();
        if (!physResult || physResult.value().empty()) {
            spdlog::error("Failed to select Vulkan physical device: {}",
                physResult ? std::string("no suitable device") : physResult.error().message());
            return;
        }

        // The DRIVER decides, not the enumeration order. The Vulkan SDK for macOS registers
        // two drivers for the same GPU, MoltenVK and Mesa's KosmicKrisp, and both report the
        // same device name, so "the first suitable device" is whichever ICD the loader
        // happened to list first. This backend is written and tested against MoltenVK (its
        // sampler limit, its MSL translation), so MoltenVK is preferred;
        // VISUTWIN_VULKAN_DRIVER=<name> picks another by a case-insensitive match on the
        // driver name ("kosmickrisp", "moltenvk").
        const auto driverProperties = [](const VkPhysicalDevice device) {
            VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &driver;
            vkGetPhysicalDeviceProperties2(device, &properties);
            return driver;
        };
        const auto lowered = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const char* requestedDriver = std::getenv("VISUTWIN_VULKAN_DRIVER");
        const std::string wanted = requestedDriver ? lowered(requestedDriver) : std::string();
        const auto& candidates = physResult.value();
        const vkb::PhysicalDevice* chosen = nullptr;
        for (const auto& candidate : candidates) {
            const auto driver = driverProperties(candidate.physical_device);
            const bool match = wanted.empty()
                ? driver.driverID == VK_DRIVER_ID_MOLTENVK
                : lowered(driver.driverName).find(wanted) != std::string::npos;
            if (match) {
                chosen = &candidate;
                break;
            }
        }
        if (!chosen) {
            if (!wanted.empty()) {
                spdlog::warn("VISUTWIN_VULKAN_DRIVER='{}' matches no suitable driver; using the first", requestedDriver);
            }
            chosen = &candidates.front();
        }
        auto vkbPhysical = *chosen;
        _physicalDevice = vkbPhysical.physical_device;

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(_physicalDevice, &props);
        const auto chosenDriver = driverProperties(_physicalDevice);
        spdlog::info("Vulkan device: {} ({} {}), apiVersion={}.{}.{}", props.deviceName,
            chosenDriver.driverName, chosenDriver.driverInfo,
            VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion));

        // Texture dimension limits, so anything sizing a texture from content —
        // the lightmappers, the skybox cube bake, a shadow map — clamps to what
        // this device actually accepts instead of to a literal 4096.
        setMaxTextureSize(static_cast<int>(props.limits.maxImageDimension2D));
        setMaxCubeMapSize(static_cast<int>(props.limits.maxImageDimensionCube));

        // Half- and full-float colour attachments are optional in Vulkan, and a
        // half-float attachment is what VSM shadows render their EVSM moments
        // into: without it Light falls the shadow type back to PCF3 rather than
        // asking for a render target the driver would refuse.
        const auto colorRenderable = [this](const VkFormat format) {
            VkFormatProperties formatProps{};
            vkGetPhysicalDeviceFormatProperties(_physicalDevice, format, &formatProps);
            return (formatProps.optimalTilingFeatures &
                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
        };
        setTextureHalfFloatRenderable(colorRenderable(VK_FORMAT_R16G16B16A16_SFLOAT));
        setTextureFloatRenderable(colorRenderable(VK_FORMAT_R32G32B32A32_SFLOAT));

        // Highest MSAA sample count a color+depth render target can use here.
        // Left at the base-class default of 1 the RenderTarget clamp silently
        // disabled every multisampled target on this backend, which is what
        // made RenderingSettings::samples a no-op under Vulkan.
        const VkSampleCountFlags framebufferSampleCounts =
            props.limits.framebufferColorSampleCounts &
            props.limits.framebufferDepthSampleCounts;
        for (const int candidate : {8, 4, 2}) {
            if (framebufferSampleCounts & vulkanSampleCountFlag(candidate)) {
                setMaxSamples(candidate);
                break;
            }
        }

        // Depth resolve is a separate capability from color resolve. The spec
        // requires SAMPLE_ZERO, but ask rather than assume: without a supported
        // mode a multisampled depth buffer simply is not resolved (nothing but
        // a later depth *sample* needs it, and the pass still antialiases).
        VkPhysicalDeviceDepthStencilResolveProperties depthResolveProps{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 props2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &depthResolveProps;
        vkGetPhysicalDeviceProperties2(_physicalDevice, &props2);
        // Depth and stencil must resolve with the SAME mode unless the device
        // advertises independentResolve, so require sample-zero in both sets and
        // resolve a combined depth-stencil attachment with one mode either way.
        _depthResolveMode =
            (depthResolveProps.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT) &&
            (depthResolveProps.supportedStencilResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
                ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                : VK_RESOLVE_MODE_NONE;
        spdlog::info("Vulkan MSAA: max {} samples, depth resolve {}", maxSamples(),
            _depthResolveMode == VK_RESOLVE_MODE_NONE ? "unsupported" : "sample-zero");
        spdlog::info("Vulkan limits: texture {}, cube {}, float renderable 16F={} 32F={}",
            maxTextureSize(), maxCubeMapSize(),
            textureHalfFloatRenderable(), textureFloatRenderable());

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
        // Capped at 16 because that is Metal's ceiling: the two backends must
        // filter at the same ratio or they disagree on every oblique surface, and
        // nothing in a frame would say which one was right.
        setMaxAnisotropy(_samplerAnisotropyEnabled
            ? std::min(16.0f, props.limits.maxSamplerAnisotropy) : 1.0f);

        // Dual-source blending (the BLENDMODE_SRC1_* factors) is an optional
        // Vulkan feature and must be enabled at device creation before
        // VK_BLEND_FACTOR_SRC1_* may appear in any pipeline. Opt in whenever the
        // hardware offers it; supportsDualSourceBlending() reports the result so
        // callers can check before building such a blend state.
        _dualSrcBlendEnabled = supported.dualSrcBlend == VK_TRUE;
        _textureCompressionAstcLdr = supported.textureCompressionASTC_LDR == VK_TRUE;
        _textureCompressionBc = supported.textureCompressionBC == VK_TRUE;
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

    std::shared_ptr<Shader> VulkanGraphicsDevice::createShaderFromCode(const ShaderDefinition& definition,
        const ShaderCode& code)
    {
        // A depth-only program may have no fragment stage at all (an ordinary shadow caster).
        const bool graphics = !code.vertexSpirv.empty() && (!code.fragmentSpirv.empty() || code.depthOnlyFragment);
        const bool compute = !code.computeSpirv.empty();
        if (!graphics && !compute) {
            spdlog::error("VulkanGraphicsDevice::createShaderFromCode('{}'): no SPIR-V for a vertex + fragment "
                "pair or a compute stage", definition.name);
            return nullptr;
        }
        // The forward family: the pipeline picks one of these vertex stages per draw by the
        // draw's vertex layout and features (VulkanRenderPipeline::create), by entry name.
        const auto family = [&code](const char* name) -> const std::vector<uint32_t>* {
            for (const auto& entry : code.vertexFamily) {
                if (entry.name == name) {
                    return &entry.words;
                }
            }
            return nullptr;
        };
        const auto data = [](const std::vector<uint32_t>* words) { return words ? words->data() : nullptr; };
        const auto size = [](const std::vector<uint32_t>* words) { return words ? words->size() : size_t{0}; };
        const auto* instanced = family("forwardInstancedVertex");
        const auto* sky = family("forwardSkyVertex");
        const auto* color = family("forwardColorVertex");
        const auto* point = family("forwardPointVertex");
        const auto* dynamicBatch = family("forwardDynamicBatchVertex");
        const auto* skinned = family("forwardSkinnedVertex");
        const auto* morphed = family("forwardMorphedVertex");
        const auto* skinnedMorphed = family("forwardSkinnedMorphedVertex");
        const bool fragment = graphics && !code.fragmentSpirv.empty();
        auto shader = std::make_shared<VulkanShader>(this, definition,
            graphics ? code.vertexSpirv.data() : nullptr, code.vertexSpirv.size(),
            fragment ? code.fragmentSpirv.data() : nullptr, fragment ? code.fragmentSpirv.size() : 0,
            data(instanced), size(instanced), data(sky), size(sky), data(color), size(color),
            data(point), size(point), data(dynamicBatch), size(dynamicBatch), data(skinned), size(skinned),
            data(morphed), size(morphed), data(skinnedMorphed), size(skinnedMorphed),
            code.specializeFeatures,
            compute ? code.computeSpirv.data() : nullptr, code.computeSpirv.size());
        if (shader && code.depthOnlyFragment && fragment) {
            shader->setDepthOnlyFragment(true);
        }
        return shader;
    }

    std::shared_ptr<Shader> VulkanGraphicsDevice::createShader(
        const ShaderDefinition& definition, const std::string& sourceCode)
    {
        // A shader from SOURCE is a custom one (ShaderMaterial, a custom quad pass, an
        // application's): GLSL compiled at run time through shaderc. The engine's own
        // programs are Slang and arrive through createShaderFromCode. MSL is Metal's and
        // fails here explicitly rather than becoming some other shader.
        if (sourceCode.empty()) {
            spdlog::error("VulkanGraphicsDevice::createShader('{}'): no source", definition.name);
            return nullptr;
        }
        if (!looksLikeGlsl(sourceCode)) {
            spdlog::error("VulkanGraphicsDevice::createShader('{}'): custom MSL cannot be used by Vulkan; "
                "provide GLSL", definition.name);
            return nullptr;
        }
        if (!vulkanShaderCompilerAvailable()) {
            spdlog::error("VulkanGraphicsDevice::createShader('{}'): runtime GLSL requires shaderc, which this "
                "build lacks", definition.name);
            return nullptr;
        }

        if (!definition.cshader.empty()) {
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

        // One source compiled twice, with VT_VERTEX_SHADER / VT_FRAGMENT_SHADER defined, so an
        // author guards the stages with #ifdef. A failed custom shader is an error.
        auto vertSpv = vulkanCompileGlsl(sourceCode, VulkanShaderStage::Vertex,
            definition.name + ".vert", {{"VT_VERTEX_SHADER", "1"}});
        auto fragSpv = vulkanCompileGlsl(sourceCode, VulkanShaderStage::Fragment,
            definition.name + ".frag", {{"VT_FRAGMENT_SHADER", "1"}});
        if (vertSpv.empty() || fragSpv.empty()) {
            spdlog::error("VulkanGraphicsDevice::createShader('{}'): custom GLSL failed", definition.name);
            return nullptr;
        }
        spdlog::info("VulkanGraphicsDevice::createShader('{}'): compiled custom GLSL at runtime", definition.name);
        return std::make_shared<VulkanShader>(this, definition,
            vertSpv.data(), vertSpv.size(),
            fragSpv.data(), fragSpv.size());
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
        const ScopedMilliseconds timer(renderTargetCreationTimeTotal());   // stats.misc
        // Caller may pass colorBuffer/depthBuffer textures that have not yet
        // had their device assigned; ensure we backfill it before
        // RenderTarget's constructor runs (it asserts on a non-null device).
        RenderTargetOptions opts = options;
        if (!opts.graphicsDevice) {
            opts.graphicsDevice = this;
        }
        return std::make_shared<VulkanRenderTarget>(opts);
    }

    bool VulkanGraphicsDevice::supportsCompressedFormat(const PixelFormat format) const
    {
        if (!isCompressedPixelFormat(format)) {
            return true;
        }
        if (_physicalDevice == VK_NULL_HANDLE) {
            return false;
        }
        switch (format) {
            case PixelFormat::PIXELFORMAT_ASTC_4x4:
            case PixelFormat::PIXELFORMAT_ASTC_5x5:
            case PixelFormat::PIXELFORMAT_ASTC_6x6:
            case PixelFormat::PIXELFORMAT_ASTC_8x8:
            case PixelFormat::PIXELFORMAT_ASTC_10x10:
            case PixelFormat::PIXELFORMAT_ASTC_12x12:
                if (!_textureCompressionAstcLdr) return false;
                break;
            case PixelFormat::PIXELFORMAT_DXT1:
            case PixelFormat::PIXELFORMAT_DXT3:
            case PixelFormat::PIXELFORMAT_DXT5:
            case PixelFormat::PIXELFORMAT_BC4:
            case PixelFormat::PIXELFORMAT_BC5:
            case PixelFormat::PIXELFORMAT_BC6H:
            case PixelFormat::PIXELFORMAT_BC7:
                if (!_textureCompressionBc) return false;
                break;
            default:
                return false;
        }
        // The feature bit promises the family; the format itself still has to be
        // sampleable with optimal tiling on this device.
        const VkFormat vkFormat = vulkanMapPixelFormat(format);
        if (vkFormat == VK_FORMAT_UNDEFINED) {
            return false;
        }
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(_physicalDevice, vkFormat, &properties);
        return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    }
}

#endif // VISUTWIN_HAS_VULKAN
