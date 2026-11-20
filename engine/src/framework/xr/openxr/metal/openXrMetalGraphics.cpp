// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// The OpenXR graphics binding for the Metal backend (XR_KHR_metal_enable).
#ifdef VISUTWIN_XR_OPENXR

#define XR_USE_GRAPHICS_API_METAL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <Metal/Metal.hpp>
#include <spdlog/spdlog.h>

#include "framework/xr/openxr/openXrGraphics.h"
#include "platform/graphics/metal/metalGraphicsDevice.h"
#include "platform/graphics/metal/metalTexture.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        /// The linear twin a format is drawn through, or the format itself.
        MTL::PixelFormat linearTwin(const MTL::PixelFormat format)
        {
            switch (format) {
            case MTL::PixelFormatRGBA8Unorm_sRGB: return MTL::PixelFormatRGBA8Unorm;
            case MTL::PixelFormatBGRA8Unorm_sRGB: return MTL::PixelFormatBGRA8Unorm;
            default: return format;
            }
        }

        class OpenXrMetalGraphics final : public OpenXrGraphics
        {
        public:
            explicit OpenXrMetalGraphics(MetalGraphicsDevice& device) : _device(device)
            {
                _binding.type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR;
                _binding.next = nullptr;
                _binding.commandQueue = device.commandQueue();
            }

            ~OpenXrMetalGraphics() override
            {
                for (MTL::Texture* view : _views) {
                    view->release();
                }
            }

            [[nodiscard]] const char* extensionName() const override { return XR_KHR_METAL_ENABLE_EXTENSION_NAME; }

            bool checkRequirements(const XrInstance instance, const XrSystemId systemId, std::string& error) override
            {
                PFN_xrGetMetalGraphicsRequirementsKHR getRequirements = nullptr;
                if (XR_FAILED(xrGetInstanceProcAddr(instance, "xrGetMetalGraphicsRequirementsKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements))) || !getRequirements) {
                    error = "the runtime does not provide xrGetMetalGraphicsRequirementsKHR";
                    return false;
                }
                XrGraphicsRequirementsMetalKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR};
                if (XR_FAILED(getRequirements(instance, systemId, &requirements))) {
                    error = "the runtime did not state its Metal requirements";
                    return false;
                }
                // The runtime names the device its compositor runs on; the session's command
                // queue has to belong to it.
                if (requirements.metalDevice && requirements.metalDevice != _device.raw()) {
                    error = "the OpenXR runtime composites on another Metal device than the engine renders with";
                    return false;
                }
                return true;
            }

            [[nodiscard]] const void* sessionBinding() override { return &_binding; }

            [[nodiscard]] int64_t chooseColorFormat(const std::vector<int64_t>& runtimeFormats) const override
            {
                // An sRGB format first (see OpenXrGraphics::chooseColorFormat); a linear one is
                // shown too dark or too bright by a compositor that takes its values as linear.
                static constexpr MTL::PixelFormat kPreferred[] = {
                    MTL::PixelFormatRGBA8Unorm_sRGB, MTL::PixelFormatBGRA8Unorm_sRGB,
                    MTL::PixelFormatRGBA8Unorm, MTL::PixelFormatBGRA8Unorm,
                };
                for (const MTL::PixelFormat wanted : kPreferred) {
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
                std::vector<XrSwapchainImageMetalKHR> runtimeImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR});
                if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, count, &count,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(runtimeImages.data())))) {
                    error = "the runtime's swapchain images could not be enumerated";
                    return false;
                }

                const auto runtimeFormat = static_cast<MTL::PixelFormat>(format);
                const MTL::PixelFormat drawFormat = linearTwin(runtimeFormat);
                const PixelFormat engineFormat = drawFormat == MTL::PixelFormatBGRA8Unorm
                    ? PixelFormat::PIXELFORMAT_BGRA8 : PixelFormat::PIXELFORMAT_RGBA8;

                for (uint32_t i = 0; i < count; ++i) {
                    auto* runtimeTexture = static_cast<MTL::Texture*>(runtimeImages[i].texture);
                    if (!runtimeTexture) {
                        error = "the runtime handed out a null swapchain texture";
                        return false;
                    }
                    // The texture the engine draws through: a linear view of an sRGB image,
                    // where the image allows views, or the image itself.
                    MTL::Texture* view = nullptr;
                    if (drawFormat != runtimeFormat && (runtimeTexture->usage() & MTL::TextureUsagePixelFormatView)) {
                        view = runtimeTexture->newTextureView(drawFormat);
                    }
                    if (!view) {
                        if (drawFormat != runtimeFormat && !_viewWarned) {
                            _viewWarned = true;
                            spdlog::warn("OpenXR: the runtime's sRGB swapchain images allow no linear view; "
                                "the views will be shown brighter than the window shows them");
                        }
                        view = runtimeTexture;
                        view->retain();
                    }
                    _views.push_back(view);

                    TextureOptions textureOptions;
                    textureOptions.name = name + "Image" + std::to_string(i);
                    textureOptions.width = width;
                    textureOptions.height = height;
                    textureOptions.format = view->pixelFormat() == MTL::PixelFormatBGRA8Unorm ||
                        view->pixelFormat() == MTL::PixelFormatBGRA8Unorm_sRGB ? PixelFormat::PIXELFORMAT_BGRA8 : engineFormat;
                    textureOptions.mipmaps = false;
                    textureOptions.minFilter = FilterMode::FILTER_LINEAR;
                    textureOptions.magFilter = FilterMode::FILTER_LINEAR;
                    OpenXrSwapchainImage image;
                    image.texture = std::make_shared<Texture>(&_device, textureOptions);
                    image.texture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
                    image.texture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

                    RenderTargetOptions targetOptions;
                    targetOptions.graphicsDevice = &_device;
                    targetOptions.colorBuffer = image.texture.get();
                    targetOptions.depth = true;
                    targetOptions.samples = samples;
                    targetOptions.name = textureOptions.name;
                    image.target = _device.createRenderTarget(targetOptions);

                    // After the target exists: building it recreates the texture's own GPU
                    // image, which the runtime's then replaces. The target finds the new
                    // texture when its next pass begins.
                    auto* hardware = dynamic_cast<gpu::MetalTexture*>(image.texture->impl());
                    if (!image.target || !hardware) {
                        error = "the device could not create a target over a swapchain image";
                        return false;
                    }
                    hardware->setExternalTexture(view);
                    images.push_back(std::move(image));
                }
                return true;
            }

            void releaseImages(std::vector<OpenXrSwapchainImage>& images) override
            {
                // The wrappers borrow the views: they go first.
                for (auto& image : images) {
                    if (auto* hardware = image.texture ? dynamic_cast<gpu::MetalTexture*>(image.texture->impl()) : nullptr) {
                        if (const auto found = std::ranges::find(_views, hardware->raw()); found != _views.end()) {
                            hardware->setExternalTexture(nullptr);
                            (*found)->release();
                            _views.erase(found);
                        }
                    }
                }
                images.clear();
            }

        private:
            MetalGraphicsDevice& _device;
            XrGraphicsBindingMetalKHR _binding{};
            std::vector<MTL::Texture*> _views;
            bool _viewWarned = false;
        };
    }

    std::unique_ptr<OpenXrGraphics> createOpenXrMetalGraphics(GraphicsDevice& device)
    {
        auto* metal = dynamic_cast<MetalGraphicsDevice*>(&device);
        return metal ? std::make_unique<OpenXrMetalGraphics>(*metal) : nullptr;
    }
}

#endif
