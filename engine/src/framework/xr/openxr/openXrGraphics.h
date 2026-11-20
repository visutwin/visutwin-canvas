// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// What the OpenXR backend needs from the graphics API a device renders with: the binding
// the session is created with, and engine render targets over the runtime's swapchain
// images. One implementation per graphics backend.
#pragma once

#ifdef VISUTWIN_XR_OPENXR

#include <memory>
#include <string>
#include <vector>

#include <openxr/openxr.h>

namespace visutwin::canvas
{
    class GraphicsDevice;
    struct GraphicsDeviceOptions;
    class RenderTarget;
    class Texture;

    /// Instance extensions the backend enables where the runtime has them. The Vulkan
    /// binding draws an sRGB swapchain through a linear view, which the image allows only
    /// when it was created with that view format listed: XR_KHR_vulkan_swapchain_format_list
    /// is how a runtime is told (asking for a mutable-format swapchain alone does not make
    /// every runtime create one).
    inline constexpr const char* kOpenXrOptionalExtensions[] = {"XR_KHR_vulkan_swapchain_format_list"};

    /// One image of a runtime swapchain as the engine draws into it.
    struct OpenXrSwapchainImage
    {
        std::shared_ptr<Texture> texture;
        std::shared_ptr<RenderTarget> target;
    };

    class OpenXrGraphics
    {
    public:
        virtual ~OpenXrGraphics() = default;

        /// The instance extension of the binding (XR_KHR_metal_enable ...).
        [[nodiscard]] virtual const char* extensionName() const = 0;

        /// The instance extensions that were enabled, the optional ones the backend asks for
        /// among them (kOpenXrOptionalExtensions): told once, before anything below is asked.
        virtual void extensionsEnabled(const std::vector<std::string>& /*extensions*/) {}

        /// A structure to chain into XrSwapchainCreateInfo::next for a swapchain of `format`,
        /// or null: valid until the next call.
        [[nodiscard]] virtual const void* swapchainCreateNext(int64_t /*format*/) { return nullptr; }

        /// Asks the runtime what it requires of the graphics device and holds the engine's
        /// against it.
        virtual bool checkRequirements(XrInstance instance, XrSystemId systemId, std::string& error) = 0;

        /// The XrGraphicsBinding* structure for XrSessionCreateInfo::next.
        [[nodiscard]] virtual const void* sessionBinding() = 0;

        /// The swapchain format to ask for, of those the runtime offers (its preference
        /// first), or 0 when none will do.
        ///
        /// The forward pass writes GAMMA-encoded colour, and a compositor reads an sRGB
        /// format as exactly that, so an sRGB format is asked for and drawn into through a
        /// view of its linear twin (the swapchain's mutable-format usage): the bytes are
        /// stored as written.
        [[nodiscard]] virtual int64_t chooseColorFormat(const std::vector<int64_t>& runtimeFormats) const = 0;

        /// Engine targets (with a depth buffer of the engine's own, `samples` multisampled)
        /// over every image of `swapchain`.
        virtual bool wrapImages(XrSwapchain swapchain, int64_t format, int width, int height, int samples,
            const std::string& name, std::vector<OpenXrSwapchainImage>& images, std::string& error) = 0;

        /// An image was acquired from its swapchain and waited for: it is the engine's to
        /// draw into, in the state the runtime hands it over in.
        virtual void imageAcquired(OpenXrSwapchainImage& /*image*/) {}

        /// The frame is drawn and not yet submitted: records whatever puts an acquired image
        /// back into the state the runtime expects at release.
        virtual void prepareRelease(OpenXrSwapchainImage& /*image*/) {}

        /// Lets go of what wrapImages took, before the swapchain is destroyed.
        virtual void releaseImages(std::vector<OpenXrSwapchainImage>& images) = 0;
    };

#ifdef VISUTWIN_HAS_METAL
    std::unique_ptr<OpenXrGraphics> createOpenXrMetalGraphics(GraphicsDevice& device);
#endif
#ifdef VISUTWIN_HAS_VULKAN
    /// Null when `device` is not the Vulkan backend's.
    std::unique_ptr<OpenXrGraphics> createOpenXrVulkanGraphics(GraphicsDevice& device);
    /// Fills the Vulkan fields of `options` from the runtime (XR_KHR_vulkan_enable): the
    /// extensions it needs and the physical device of `systemId`.
    void configureOpenXrVulkanDevice(XrInstance instance, XrSystemId systemId, GraphicsDeviceOptions& options);
    const char* openXrVulkanExtensionName();
    /// An engine texture and target (with a depth buffer of the engine's own) over one image
    /// of a runtime swapchain: `image` is a VkImage created in `format` (a VkFormat) for
    /// mutable formats, drawn through the linear twin of an sRGB format. What wrapImages does
    /// per image; apart from it for a test that plays the runtime. An empty result on failure.
    OpenXrSwapchainImage wrapOpenXrVulkanImage(GraphicsDevice& device, void* image, int64_t format, int width,
        int height, int samples, const std::string& name);
#endif
}

#endif
