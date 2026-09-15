// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//

#ifdef VISUTWIN_HAS_VULKAN

#include "vulkanRenderTarget.h"

#include "vulkanGraphicsDevice.h"
#include "vulkanTexture.h"
#include "vulkanUtils.h"

#include "platform/graphics/texture.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // Build a VkImageView that targets a single (face, mipLevel) of the
        // texture's VkImage.  For the trivial case (face 0, mip 0, one layer, one
        // level) we return the texture's own primary view to avoid the extra alloc.
        //
        // A MIPMAPPED texture needs a carved view even at mip 0: the primary view
        // spans every level, and an attachment view puts every subresource it
        // covers into the attachment layout. Only level 0 is barriered, so levels
        // 1+ were left in COLOR_ATTACHMENT_OPTIMAL behind the layout tracker's back,
        // and the mip generation that follows the pass barriered them from a layout
        // they were not in.
        VkImageView resolveAttachmentView(VkDevice device,
            gpu::VulkanTexture* tex, int face, int mipLevel,
            bool& outOwn)
        {
            outOwn = false;
            if (!tex || tex->image() == VK_NULL_HANDLE) {
                return VK_NULL_HANDLE;
            }

            const bool needsCarvedView = (face != 0) || (mipLevel != 0) ||
                                         (tex->arrayLayers() > 1) ||
                                         (tex->mipLevels() > 1);

            if (!needsCarvedView) {
                return tex->imageView();
            }

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = tex->image();
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = tex->format();
            viewInfo.subresourceRange.aspectMask = tex->aspect();
            viewInfo.subresourceRange.baseMipLevel = static_cast<uint32_t>(mipLevel);
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = static_cast<uint32_t>(face);
            viewInfo.subresourceRange.layerCount = 1;

            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(device, &viewInfo, nullptr, &view) != VK_SUCCESS) {
                spdlog::error("VulkanRenderTarget: failed to carve attachment view "
                              "(face={}, mip={})", face, mipLevel);
                return VK_NULL_HANDLE;
            }
            outOwn = true;
            return view;
        }

        // One multisampled attachment surface: an image that is only ever a
        // render target (never sampled, never copied) plus its view.
        bool createMsaaSurface(VkDevice vk, VmaAllocator allocator,
            VkFormat format, uint32_t width, uint32_t height,
            VkSampleCountFlagBits samples, VkImageUsageFlags usage,
            VkImageAspectFlags aspect, const char* what,
            VkImage& outImage, VmaAllocation& outAllocation, VkImageView& outView)
        {
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = {width, height, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = samples;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = usage;
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &outImage,
                               &outAllocation, nullptr) != VK_SUCCESS) {
                spdlog::error("VulkanRenderTarget: failed to allocate multisampled {} image", what);
                outImage = VK_NULL_HANDLE;
                outAllocation = VK_NULL_HANDLE;
                return false;
            }

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = outImage;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = aspect;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(vk, &viewInfo, nullptr, &outView) != VK_SUCCESS) {
                spdlog::error("VulkanRenderTarget: failed to create multisampled {} view", what);
                vmaDestroyImage(allocator, outImage, outAllocation);
                outImage = VK_NULL_HANDLE;
                outAllocation = VK_NULL_HANDLE;
                outView = VK_NULL_HANDLE;
                return false;
            }
            return true;
        }
    }

    VulkanRenderTarget::VulkanRenderTarget(const RenderTargetOptions& options)
        : RenderTarget(options)
    {
        // Match the Metal RT contract: attachments are resolved at construction
        // time so the first render pass can begin immediately.
        createFrameBuffers();
    }

    VulkanRenderTarget::~VulkanRenderTarget()
    {
        destroyFrameBuffers();
    }

    VulkanGraphicsDevice* VulkanRenderTarget::vulkanDevice() const
    {
        return dynamic_cast<VulkanGraphicsDevice*>(device());
    }

    void VulkanRenderTarget::destroyFrameBuffers()
    {
        auto* vkDev = vulkanDevice();
        VkDevice vk = vkDev ? vkDev->device() : VK_NULL_HANDLE;
        VmaAllocator allocator = vkDev ? vkDev->vmaAllocator() : VK_NULL_HANDLE;

        // Collect handles and defer their destruction: in-flight frames may
        // still render to / sample these attachments.
        std::vector<VkImageView> views;
        // Images this RT owns outright: the internal depth buffer and every
        // multisampled surface. Paired with their allocations so one deferred
        // lambda frees them all.
        std::vector<std::pair<VkImage, VmaAllocation>> images;
        if (vk != VK_NULL_HANDLE) {
            for (auto& a : _colorAttachments) {
                if (a.ownView && a.view != VK_NULL_HANDLE) {
                    views.push_back(a.view);
                }
                if (a.msaaView != VK_NULL_HANDLE) {
                    views.push_back(a.msaaView);
                }
                if (a.msaaImage != VK_NULL_HANDLE) {
                    images.emplace_back(a.msaaImage, a.msaaAllocation);
                }
            }
            if (_depthAttachment.ownView && _depthAttachment.view != VK_NULL_HANDLE) {
                views.push_back(_depthAttachment.view);
            }
            if (_depthAttachment.msaaView != VK_NULL_HANDLE) {
                views.push_back(_depthAttachment.msaaView);
            }
            if (_depthAttachment.msaaImage != VK_NULL_HANDLE) {
                images.emplace_back(_depthAttachment.msaaImage,
                    _depthAttachment.msaaAllocation);
            }
            if (_depthAttachment.internalImage != VK_NULL_HANDLE) {
                images.emplace_back(_depthAttachment.internalImage,
                    _depthAttachment.internalAllocation);
            }
        }

        if (vkDev && (!views.empty() || !images.empty())) {
            vkDev->deferDestroy([vk, allocator, views = std::move(views), images = std::move(images)] {
                for (VkImageView view : views) {
                    vkDestroyImageView(vk, view, nullptr);
                }
                if (allocator != VK_NULL_HANDLE) {
                    for (const auto& [image, allocation] : images) {
                        vmaDestroyImage(allocator, image, allocation);
                    }
                }
            });
        }

        _colorAttachments.clear();
        _depthAttachment = VulkanDepthAttachment{};
    }

    VkSampleCountFlagBits VulkanRenderTarget::sampleCountFlag() const
    {
        return vulkanSampleCountFlag(samples());
    }

    bool VulkanRenderTarget::createMultisampledSurfaces(const VkFormat internalDepthFormat)
    {
        auto* vkDev = vulkanDevice();
        VkDevice vk = vkDev->device();
        VmaAllocator allocator = vkDev->vmaAllocator();
        const VkSampleCountFlagBits sampleFlag = sampleCountFlag();
        const uint32_t w = static_cast<uint32_t>(width());
        const uint32_t h = static_cast<uint32_t>(height());
        const VkExtent3D extent{w, h, 1};

        // The device-wide limit says a framebuffer can carry this many samples;
        // whether THIS format can is a per-format question, and an image
        // creation that violates it is undefined behaviour rather than an error
        // return. Ask first, and fall back to a single sample if the answer is
        // no — a target quietly rendering unantialiased beats one that does not
        // render at all.
        const auto supports = [&](const VkFormat format, const VkImageUsageFlags usage,
            const VkFormatFeatureFlags feature) {
            return vulkanFormatSupportsImage(vkDev->physicalDevice(), format,
                VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, feature,
                extent, 1, 1, sampleFlag);
        };

        for (const auto& att : _colorAttachments) {
            if (!supports(att.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {
                spdlog::warn("VulkanRenderTarget '{}': color format {} does not support {} "
                             "samples — falling back to 1", name(),
                             static_cast<int>(att.format), samples());
                return false;
            }
        }

        const VkFormat depthFormat = _depthAttachment.texture
            ? _depthAttachment.format : internalDepthFormat;
        if (depthFormat != VK_FORMAT_UNDEFINED &&
            !supports(depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            spdlog::warn("VulkanRenderTarget '{}': depth format {} does not support {} "
                         "samples — falling back to 1", name(),
                         static_cast<int>(depthFormat), samples());
            return false;
        }

        // Nothing below has been recorded into a command buffer yet, so the
        // unwind on failure can destroy immediately rather than defer.
        const auto unwind = [&] {
            for (auto& att : _colorAttachments) {
                if (att.msaaView != VK_NULL_HANDLE) {
                    vkDestroyImageView(vk, att.msaaView, nullptr);
                }
                if (att.msaaImage != VK_NULL_HANDLE) {
                    vmaDestroyImage(allocator, att.msaaImage, att.msaaAllocation);
                }
                att.msaaView = VK_NULL_HANDLE;
                att.msaaImage = VK_NULL_HANDLE;
                att.msaaAllocation = VK_NULL_HANDLE;
                att.msaaLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
        };

        for (auto& att : _colorAttachments) {
            if (!createMsaaSurface(vk, allocator, att.format, w, h, sampleFlag,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT, "color",
                                   att.msaaImage, att.msaaAllocation, att.msaaView)) {
                unwind();
                return false;
            }
            att.msaaLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }

        // Only a texture-backed depth buffer gets a multisampled twin — the
        // texture stays the single-sample resolve destination something later
        // samples. Internally-owned depth is created multisampled outright.
        if (_depthAttachment.texture && _depthAttachment.view != VK_NULL_HANDLE) {
            const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT |
                (vulkanFormatHasStencil(_depthAttachment.format)
                    ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            if (!createMsaaSurface(vk, allocator, _depthAttachment.format, w, h, sampleFlag,
                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                   aspect, "depth",
                                   _depthAttachment.msaaImage,
                                   _depthAttachment.msaaAllocation,
                                   _depthAttachment.msaaView)) {
                unwind();
                return false;
            }
            _depthAttachment.msaaLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }

        return true;
    }

    void VulkanRenderTarget::createFrameBuffers()
    {
        auto* vkDev = vulkanDevice();
        if (!vkDev) {
            spdlog::error("VulkanRenderTarget requires VulkanGraphicsDevice");
            return;
        }

        // Re-entrant call (e.g. on resize) — start from clean state.
        destroyFrameBuffers();

        VkDevice vk = vkDev->device();
        VmaAllocator allocator = vkDev->vmaAllocator();

        // Color attachments
        const int colorCount = colorBufferCount();
        _colorAttachments.reserve(static_cast<size_t>(colorCount));

        for (int i = 0; i < colorCount; ++i) {
            Texture* colorBuffer = getColorBuffer(i);
            if (!colorBuffer) {
                continue;
            }
            colorBuffer->upload();

            auto* vkTex = dynamic_cast<gpu::VulkanTexture*>(colorBuffer->impl());
            if (!vkTex || vkTex->image() == VK_NULL_HANDLE) {
                spdlog::warn("VulkanRenderTarget: color buffer {} has no VkImage", i);
                continue;
            }
            if (!vkTex->supportsColorAttachment()) {
                spdlog::error(
                    "VulkanRenderTarget: color buffer {} format does not support attachment usage",
                    i);
                continue;
            }

            VulkanColorAttachment attachment{};
            attachment.format = vkTex->format();
            attachment.texture = vkTex;
            attachment.view = resolveAttachmentView(vk, vkTex, face(), mipLevel(),
                                                    attachment.ownView);
            if (attachment.view == VK_NULL_HANDLE) {
                continue;
            }
            _colorAttachments.push_back(attachment);
        }

        // Depth attachment — three cases:
        //   1. options.depthBuffer set → use that texture's view.
        //   2. options.depth = true, no depthBuffer → allocate an internal depth image.
        //   3. neither → no depth attachment.
        //
        // Case 2's image is created at the END of this function rather than
        // here: its sample count has to be the one the multisampled surfaces
        // below actually settled on, not the one that was requested.
        VkFormat internalDepthFormat = VK_FORMAT_UNDEFINED;
        if (Texture* depthTex = depthBuffer()) {
            depthTex->upload();
            auto* vkTex = dynamic_cast<gpu::VulkanTexture*>(depthTex->impl());
            if (vkTex && vkTex->image() != VK_NULL_HANDLE) {
                _depthAttachment.format = vkTex->format();
                _depthAttachment.texture = vkTex;
                _depthAttachment.view = resolveAttachmentView(vk, vkTex, face(), mipLevel(),
                                                              _depthAttachment.ownView);
                _depthAttachment.currentLayout = vkTex->layout(
                    static_cast<uint32_t>(mipLevel()),
                    vkTex->arrayLayers() > 1 ? static_cast<uint32_t>(face()) : 0u);
            } else {
                spdlog::warn("VulkanRenderTarget: depthBuffer has no VkImage");
            }
        } else if (hasDepth()) {
            // D24S8 is not universally supported (MoltenVK on Apple GPUs
            // lacks it) — probe and fall back to D32S8.
            internalDepthFormat = hasStencil()
                ? vulkanSupportedDepthStencilFormat(vkDev->physicalDevice())
                : vulkanSupportedDepthFormat(vkDev->physicalDevice());
            if (internalDepthFormat == VK_FORMAT_UNDEFINED) {
                spdlog::error(
                    "VulkanRenderTarget: no supported depth-stencil attachment format");
                return;
            }
        }

        if (samples() > 1 && !createMultisampledSurfaces(internalDepthFormat)) {
            setSamples(1);
        }

        if (internalDepthFormat != VK_FORMAT_UNDEFINED) {
            const uint32_t w = static_cast<uint32_t>(width());
            const uint32_t h = static_cast<uint32_t>(height());

            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = internalDepthFormat;
            imageInfo.extent = {w, h, 1};
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            // Internal depth is the pass's own scratch buffer — nothing samples
            // it, so a multisampled target needs no resolved twin here.
            imageInfo.samples = sampleCountFlag();
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            // TRANSFER_SRC so grabSceneDepth can copy this buffer out for SSR.
            // A multisampled image cannot be a transfer source, and nothing can
            // grab depth out of one anyway.
            imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                (samples() > 1 ? 0 : VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            if (vmaCreateImage(allocator, &imageInfo, &allocInfo,
                               &_depthAttachment.internalImage,
                               &_depthAttachment.internalAllocation, nullptr) == VK_SUCCESS) {
                VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                viewInfo.image = _depthAttachment.internalImage;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = internalDepthFormat;
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                    (hasStencil() ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
                viewInfo.subresourceRange.baseMipLevel = 0;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = 1;
                const VkResult viewResult =
                    vkCreateImageView(vk, &viewInfo, nullptr, &_depthAttachment.view);
                if (viewResult != VK_SUCCESS) {
                    spdlog::error(
                        "VulkanRenderTarget: failed to create internal depth image view ({})",
                        static_cast<int>(viewResult));
                    vmaDestroyImage(allocator, _depthAttachment.internalImage,
                        _depthAttachment.internalAllocation);
                    _depthAttachment.internalImage = VK_NULL_HANDLE;
                    _depthAttachment.internalAllocation = VK_NULL_HANDLE;
                    return;
                }

                _depthAttachment.format = internalDepthFormat;
                _depthAttachment.ownView = true;
                _depthAttachment.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            } else {
                spdlog::error("VulkanRenderTarget: failed to allocate internal depth image");
            }
        }
    }
}

#endif // VISUTWIN_HAS_VULKAN
