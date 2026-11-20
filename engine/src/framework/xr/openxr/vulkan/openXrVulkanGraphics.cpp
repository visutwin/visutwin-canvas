// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 20.11.2026
//
// The OpenXR graphics binding for the Vulkan backend (XR_KHR_vulkan_enable).
//
// The runtime's compositor shares the engine's Vulkan device, so it has a say in how that
// device is made: configureOpenXrVulkanDevice asks it for the instance and device extensions
// it needs and for the physical device of the headset, before the device exists. Its swapchain
// images are then drawn into directly, each wrapped in an engine texture and target.
#ifdef VISUTWIN_XR_OPENXR

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <span>
#include <sstream>

#include <spdlog/spdlog.h>

#include "framework/xr/openxr/openXrGraphics.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "platform/graphics/vulkan/vulkanGraphicsDevice.h"
#include "platform/graphics/vulkan/vulkanTexture.h"

namespace visutwin::canvas
{
    namespace
    {
        template <typename Function>
        Function runtimeFunction(const XrInstance instance, const char* name)
        {
            Function function = nullptr;
            if (XR_FAILED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&function)))) {
                return nullptr;
            }
            return function;
        }

        /// The names in one of the runtime's space-separated extension lists.
        template <typename Function>
        std::vector<std::string> extensionList(const Function function, const XrInstance instance, const XrSystemId systemId)
        {
            std::vector<std::string> names;
            uint32_t size = 0;
            if (!function || XR_FAILED(function(instance, systemId, 0, &size, nullptr)) || size == 0) {
                return names;
            }
            std::string buffer(size, '\0');
            if (XR_FAILED(function(instance, systemId, size, &size, buffer.data()))) {
                return names;
            }
            std::istringstream stream(buffer.c_str());
            for (std::string name; stream >> name;) {
                names.push_back(name);
            }
            return names;
        }

        VkPhysicalDevice runtimePhysicalDevice(const XrInstance instance, const XrSystemId systemId, const VkInstance vkInstance)
        {
            const auto getDevice = runtimeFunction<PFN_xrGetVulkanGraphicsDeviceKHR>(instance, "xrGetVulkanGraphicsDeviceKHR");
            VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
            if (!getDevice || XR_FAILED(getDevice(instance, systemId, vkInstance, &physicalDevice))) {
                return VK_NULL_HANDLE;
            }
            return physicalDevice;
        }

        /// The linear twin a format is drawn through, or the format itself.
        VkFormat linearTwin(const VkFormat format)
        {
            switch (format) {
            case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
            case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
            default: return format;
            }
        }

        class OpenXrVulkanGraphics final : public OpenXrGraphics
        {
        public:
            explicit OpenXrVulkanGraphics(VulkanGraphicsDevice& device) : _device(device)
            {
                _binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
                _binding.next = nullptr;
                _binding.instance = device.instance();
                _binding.physicalDevice = device.physicalDevice();
                _binding.device = device.device();
                _binding.queueFamilyIndex = device.graphicsQueueFamily();
                // The engine takes the family's first queue.
                _binding.queueIndex = 0;
            }

            [[nodiscard]] const char* extensionName() const override { return XR_KHR_VULKAN_ENABLE_EXTENSION_NAME; }

            bool checkRequirements(const XrInstance instance, const XrSystemId systemId, std::string& error) override
            {
                const auto getRequirements = runtimeFunction<PFN_xrGetVulkanGraphicsRequirementsKHR>(instance,
                    "xrGetVulkanGraphicsRequirementsKHR");
                XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
                // The call is owed before a session is created, whatever it answers.
                if (!getRequirements || XR_FAILED(getRequirements(instance, systemId, &requirements))) {
                    error = "the runtime did not state its Vulkan requirements";
                    return false;
                }
                constexpr XrVersion engineVersion = XR_MAKE_VERSION(1, 3, 0);
                if (requirements.minApiVersionSupported > engineVersion) {
                    error = "the OpenXR runtime needs a newer Vulkan than the engine's 1.3";
                    return false;
                }
                // The session's device has to be the one the runtime composites on, with the
                // extensions it asked for: both are settled when the device is created.
                const VkPhysicalDevice required = runtimePhysicalDevice(instance, systemId, _device.instance());
                if (required != VK_NULL_HANDLE && required != _device.physicalDevice()) {
                    error = "the OpenXR runtime composites on another GPU than the engine's device: create the "
                        "graphics device after XrBackend::configureGraphicsDevice";
                    return false;
                }
                return true;
            }

            [[nodiscard]] const void* sessionBinding() override { return &_binding; }

            void extensionsEnabled(const std::vector<std::string>& extensions) override
            {
                _formatList = std::ranges::find(extensions, std::string(XR_KHR_VULKAN_SWAPCHAIN_FORMAT_LIST_EXTENSION_NAME)) !=
                    extensions.end();
                if (!_formatList) {
                    spdlog::warn("OpenXR: the runtime has no XR_KHR_vulkan_swapchain_format_list, so its sRGB images "
                        "cannot be drawn through a linear view; a linear swapchain format is used, which a "
                        "compositor may show with the wrong gamma");
                }
            }

            [[nodiscard]] const void* swapchainCreateNext(const int64_t format) override
            {
                // The image's own format and the linear twin it is drawn through.
                _viewFormats[0] = static_cast<VkFormat>(format);
                _viewFormats[1] = linearTwin(_viewFormats[0]);
                if (!_formatList || _viewFormats[0] == _viewFormats[1]) {
                    return nullptr;
                }
                _formatListInfo = {XR_TYPE_VULKAN_SWAPCHAIN_FORMAT_LIST_CREATE_INFO_KHR};
                _formatListInfo.viewFormatCount = 2;
                _formatListInfo.viewFormats = _viewFormats;
                return &_formatListInfo;
            }

            [[nodiscard]] int64_t chooseColorFormat(const std::vector<int64_t>& runtimeFormats) const override
            {
                // An sRGB format first (see OpenXrGraphics::chooseColorFormat), where its
                // linear view can be asked for; otherwise a linear format, which needs none.
                static constexpr VkFormat kWithView[] = {
                    VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
                    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
                };
                static constexpr VkFormat kWithoutView[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
                const std::span<const VkFormat> preferred = _formatList ? std::span<const VkFormat>(kWithView)
                    : std::span<const VkFormat>(kWithoutView);
                for (const VkFormat wanted : preferred) {
                    for (const int64_t offered : runtimeFormats) {
                        if (offered == static_cast<int64_t>(wanted)) {
                            return offered;
                        }
                    }
                }
                return 0;
            }

            bool wrapImages(const XrSwapchain swapchain, const int64_t format, const int width, const int height,
                const int samples, const std::string& name, std::vector<OpenXrSwapchainImage>& images,
                std::string& error) override
            {
                uint32_t count = 0;
                if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr)) || count == 0) {
                    error = "the runtime's swapchain has no images";
                    return false;
                }
                std::vector<XrSwapchainImageVulkanKHR> runtimeImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
                if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, count, &count,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(runtimeImages.data())))) {
                    error = "the runtime's swapchain images could not be enumerated";
                    return false;
                }

                for (uint32_t i = 0; i < count; ++i) {
                    OpenXrSwapchainImage image = wrapOpenXrVulkanImage(_device, runtimeImages[i].image, format, width,
                        height, samples, name + "Image" + std::to_string(i));
                    if (!image.target) {
                        error = "the device could not create a target over a swapchain image";
                        return false;
                    }
                    images.push_back(std::move(image));
                }
                return true;
            }

            void imageAcquired(OpenXrSwapchainImage& image) override
            {
                // A colour image comes from the runtime as a colour attachment.
                if (auto* hardware = image.texture ? dynamic_cast<gpu::VulkanTexture*>(image.texture->impl()) : nullptr) {
                    hardware->noteExternalLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                }
            }

            void prepareRelease(OpenXrSwapchainImage& image) override
            {
                // ... and goes back as one. The frame's passes and the window mirror left it
                // in whatever layout they last used it in.
                auto* hardware = image.texture ? dynamic_cast<gpu::VulkanTexture*>(image.texture->impl()) : nullptr;
                if (hardware && _device.recording()) {
                    hardware->transitionLayout(_device.currentCommandBuffer(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                }
            }

            void releaseImages(std::vector<OpenXrSwapchainImage>& images) override
            {
                // The views die with the wrappers, through the device's deferred destroys:
                // nothing draws into a swapchain that is about to go.
                images.clear();
            }

        private:
            VulkanGraphicsDevice& _device;
            XrGraphicsBindingVulkanKHR _binding{};
            bool _formatList = false;
            VkFormat _viewFormats[2] = {VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
            XrVulkanSwapchainFormatListCreateInfoKHR _formatListInfo{};
        };
    }

    OpenXrSwapchainImage wrapOpenXrVulkanImage(GraphicsDevice& device, void* image, const int64_t format,
        const int width, const int height, const int samples, const std::string& name)
    {
        auto* vulkan = dynamic_cast<VulkanGraphicsDevice*>(&device);
        if (!vulkan || !image) {
            return {};
        }
        // Drawn through the linear twin of an sRGB format: the swapchain is created for
        // mutable formats, so the view is legal and the bytes are stored as written.
        const VkFormat viewFormat = linearTwin(static_cast<VkFormat>(format));
        TextureOptions textureOptions;
        textureOptions.name = name;
        textureOptions.width = width;
        textureOptions.height = height;
        textureOptions.format = viewFormat == VK_FORMAT_B8G8R8A8_UNORM
            ? PixelFormat::PIXELFORMAT_BGRA8 : PixelFormat::PIXELFORMAT_RGBA8;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        OpenXrSwapchainImage wrapped;
        wrapped.texture = std::make_shared<Texture>(vulkan, textureOptions);
        wrapped.texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        wrapped.texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = vulkan;
        targetOptions.colorBuffer = wrapped.texture.get();
        targetOptions.depth = true;
        targetOptions.samples = samples;
        targetOptions.name = name;
        wrapped.target = vulkan->createRenderTarget(targetOptions);

        // After the target exists: building it recreates the texture's own image, which the
        // runtime's then replaces, and the target takes new views of it. The layout is set
        // for real each time the image is acquired.
        auto* hardware = dynamic_cast<gpu::VulkanTexture*>(wrapped.texture->impl());
        if (!wrapped.target || !hardware ||
            !hardware->adoptExternalImage(vulkan, static_cast<VkImage>(image), viewFormat, VK_IMAGE_LAYOUT_UNDEFINED)) {
            return {};
        }
        wrapped.target->refreshAttachments();
        return wrapped;
    }

    const char* openXrVulkanExtensionName()
    {
        return XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
    }

    void configureOpenXrVulkanDevice(const XrInstance instance, const XrSystemId systemId, GraphicsDeviceOptions& options)
    {
        const auto instanceExtensions = extensionList(
            runtimeFunction<PFN_xrGetVulkanInstanceExtensionsKHR>(instance, "xrGetVulkanInstanceExtensionsKHR"),
            instance, systemId);
        const auto deviceExtensions = extensionList(
            runtimeFunction<PFN_xrGetVulkanDeviceExtensionsKHR>(instance, "xrGetVulkanDeviceExtensionsKHR"),
            instance, systemId);
        options.vulkanInstanceExtensions.insert(options.vulkanInstanceExtensions.end(),
            instanceExtensions.begin(), instanceExtensions.end());
        options.vulkanDeviceExtensions.insert(options.vulkanDeviceExtensions.end(),
            deviceExtensions.begin(), deviceExtensions.end());
        options.vulkanPhysicalDevice = [instance, systemId](void* vulkanInstance) -> void* {
            return runtimePhysicalDevice(instance, systemId, static_cast<VkInstance>(vulkanInstance));
        };
        spdlog::info("OpenXR asks the Vulkan device for {} instance and {} device extension(s)",
            instanceExtensions.size(), deviceExtensions.size());
    }

    std::unique_ptr<OpenXrGraphics> createOpenXrVulkanGraphics(GraphicsDevice& device)
    {
        auto* vulkan = dynamic_cast<VulkanGraphicsDevice*>(&device);
        return vulkan ? std::make_unique<OpenXrVulkanGraphics>(*vulkan) : nullptr;
    }
}

#endif
