// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// An XR session on a real device of this build's backend, through the simulated backend:
// the engine's own update and render drive it, and the views' images are read back.
//
// Stereo: a small box straight ahead of the head must land right of centre in the left
// eye's image and left of centre in the right eye's, by the amount the eye separation, the
// distance and the field of view give, and above centre by its height over the head. That
// holds the per-view matrices, the per-view targets and the image's orientation at once.
//
// Depth slices: with the depth range cut in two, a near box must be in the near slice only
// and a far box in the far slice only; the near slice is cleared to transparent black and
// the far one to the camera's clear colour.
//
// Also: the camera's "prerender" fires once a frame however many views are drawn, input
// sources fire their press events, and ending the session gives the camera back.
//
// It needs a GPU and a window, so it carries the `gpu` label, which the CI test presets
// exclude; run it with `ctest --preset gpu` (or `ctest -L gpu` in a Vulkan build).
//
// The metal-cpp *_PRIVATE_IMPLEMENTATION translation unit of this executable: a program
// linking the static engine supplies it (see examples/exampleApp.cpp).
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/components/render/renderComponent.h"
#include "framework/components/render/renderComponentSystem.h"
#include "framework/xr/simulatedXrBackend.h"
#include "framework/xr/xrManager.h"
#include "platform/graphics/graphicsDeviceCreate.h"
#include "platform/graphics/texture.h"
#include "scene/materials/standardMaterial.h"
#include "scene/scene.h"
#include "support/check.h"
#include "support/testEngine.h"

#if defined(VISUTWIN_XR_OPENXR) && defined(VISUTWIN_HAS_VULKAN)
#include <vulkan/vulkan.h>

#include "framework/xr/openxr/openXrGraphics.h"
#include "platform/graphics/vulkan/vulkanGraphicsDevice.h"
#include "platform/graphics/vulkan/vulkanTexture.h"
#define VISUTWIN_XR_TEST_RUNTIME_IMAGES
#endif

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    constexpr int kSize = 128;

    struct Blob
    {
        int count = 0;
        double x = 0.0;
        double y = 0.0;
    };

    /// The centroid of the pixels where channel `channel` is bright and the other two dark.
    Blob findBlob(Texture* texture, const int channel)
    {
        Blob blob;
        std::vector<uint8_t> pixels;
        if (!texture || !texture->read(pixels) || pixels.size() < static_cast<size_t>(kSize * kSize * 4)) {
            return blob;
        }
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                const uint8_t* p = &pixels[static_cast<size_t>((y * kSize + x) * 4)];
                bool match = p[channel] > 160;
                for (int other = 0; other < 3; ++other) {
                    match = match && (other == channel || p[other] < 90);
                }
                if (match) {
                    ++blob.count;
                    blob.x += x + 0.5;
                    blob.y += y + 0.5;
                }
            }
        }
        if (blob.count > 0) {
            blob.x /= blob.count;
            blob.y /= blob.count;
        }
        return blob;
    }

    std::array<int, 4> cornerPixel(Texture* texture)
    {
        std::vector<uint8_t> pixels;
        if (!texture || !texture->read(pixels, 1, 1, 1, 1) || pixels.size() < 4) {
            return {-1, -1, -1, -1};
        }
        return {pixels[0], pixels[1], pixels[2], pixels[3]};
    }

    std::shared_ptr<StandardMaterial> emissive(const Color& colour)
    {
        auto material = std::make_shared<StandardMaterial>();
        // unlit draws base + emissive: the colour is the emissive alone
        material->setDiffuse(Color(0.0f, 0.0f, 0.0f, 1.0f));
        material->setEmissive(colour);
        material->setUnlit(true);
        return material;
    }

#ifdef VISUTWIN_XR_TEST_RUNTIME_IMAGES
    /// An image as an OpenXR runtime's Vulkan swapchain holds one: sRGB, created for mutable
    /// formats, owned by nothing in the engine.
    struct RuntimeImage
    {
        VkDevice device = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;

        bool create(VulkanGraphicsDevice& vulkan, const int size)
        {
            device = vulkan.device();
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_R8G8B8A8_SRGB;
            info.extent = {static_cast<uint32_t>(size), static_cast<uint32_t>(size), 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
                return false;
            }
            VkMemoryRequirements requirements;
            vkGetImageMemoryRequirements(device, image, &requirements);
            VkPhysicalDeviceMemoryProperties properties;
            vkGetPhysicalDeviceMemoryProperties(vulkan.physicalDevice(), &properties);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            for (uint32_t type = 0; type < properties.memoryTypeCount; ++type) {
                if ((requirements.memoryTypeBits & (1u << type)) &&
                    (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    allocation.memoryTypeIndex = type;
                    break;
                }
            }
            return vkAllocateMemory(device, &allocation, nullptr, &memory) == VK_SUCCESS &&
                vkBindImageMemory(device, image, memory, 0) == VK_SUCCESS;
        }

        void destroy()
        {
            if (image != VK_NULL_HANDLE) {
                vkDestroyImage(device, image, nullptr);
            }
            if (memory != VK_NULL_HANDLE) {
                vkFreeMemory(device, memory, nullptr);
            }
        }
    };

    /// The simulated headset, drawing into images wrapped as the OpenXR Vulkan binding wraps
    /// a runtime's: what a session through OpenXR does to the device, without a runtime.
    class RuntimeImageBackend final : public SimulatedXrBackend
    {
    public:
        RuntimeImageBackend(const SimulatedXrOptions& options, OpenXrGraphics* graphics,
            std::vector<OpenXrSwapchainImage>* images)
            : SimulatedXrBackend(options), _graphics(graphics), _images(images) {}

        bool beginFrame(std::vector<XrViewTarget>& targets) override
        {
            std::vector<XrViewTarget> own;
            SimulatedXrBackend::beginFrame(own);
            for (auto& image : *_images) {
                // The first time the images are as they were created; after that, as the
                // runtime would hand them back: colour attachments.
                if (_acquired) {
                    _graphics->imageAcquired(image);
                }
                targets.push_back({image.target, Vector4(0.0f, 0.0f, kSize, kSize)});
            }
            _acquired = true;
            return true;
        }

        void finishFrame() override
        {
            for (auto& image : *_images) {
                _graphics->prepareRelease(image);
                auto* hardware = dynamic_cast<gpu::VulkanTexture*>(image.texture->impl());
                releasedAsAttachment = releasedAsAttachment && hardware &&
                    hardware->layout(0, 0) == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            ++finished;
        }

        [[nodiscard]] Texture* viewTexture(const int view, int /*slice*/) const override
        {
            return view >= 0 && static_cast<size_t>(view) < _images->size() ? (*_images)[static_cast<size_t>(view)].texture.get() : nullptr;
        }

        int finished = 0;
        bool releasedAsAttachment = true;

    private:
        OpenXrGraphics* _graphics;
        std::vector<OpenXrSwapchainImage>* _images;
        bool _acquired = false;
    };
#endif

    Entity* addBox(Engine* engine, const Vector3& position, const float size, Material* material)
    {
        auto* entity = addTo(engine->root(), newEntity(engine, "box"));
        entity->setPosition(position.getX(), position.getY(), position.getZ());
        entity->setLocalScale(size, size, size);
        auto* render = static_cast<RenderComponent*>(entity->addComponent<RenderComponent>());
        render->setType("box");
        render->setMaterial(material);
        return entity;
    }
}

int main()
{
    const Backend backend = defaultBackend();
    if (backend == Backend::Metal) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }
    const SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | (backend == Backend::Vulkan ? SDL_WINDOW_VULKAN : SDL_WINDOW_METAL);
    SDL_Window* window = SDL_CreateWindow("xr", 256, 128, flags);
    SDL_Renderer* renderer = backend == Backend::Metal ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!window || (backend == Backend::Metal && !renderer)) {
        std::cerr << "window or renderer failed: " << SDL_GetError() << '\n';
        return 1;
    }

    {
        GraphicsDeviceOptions deviceOptions;
        deviceOptions.backend = backend;
        deviceOptions.window = window;
        if (renderer) {
            deviceOptions.swapChain = SDL_GetRenderMetalLayer(renderer);
        }
        // What an OpenXR runtime asks of a Vulkan device: extensions to enable where the
        // driver has them (one real, one not), and a say in the physical device.
        int physicalDeviceAsked = 0;
        deviceOptions.vulkanInstanceExtensions = {"VK_KHR_get_physical_device_properties2", "VK_VISUTWIN_no_such_extension"};
        deviceOptions.vulkanDeviceExtensions = {"VK_KHR_maintenance1", "VK_VISUTWIN_no_such_extension"};
        deviceOptions.vulkanPhysicalDevice = [&](void* instance) -> void* {
            physicalDeviceAsked += instance != nullptr ? 1 : 0;
            return nullptr;
        };
        std::shared_ptr<GraphicsDevice> device = createGraphicsDevice(deviceOptions);
        check(device != nullptr, std::string("a ") + backendName(backend) + " device is created");
        if (!device) {
            return 1;
        }
        check(physicalDeviceAsked == (backend == Backend::Vulkan ? 1 : 0),
            "a Vulkan device asks once which physical device to use, and is created with extensions it was asked for");

        // A head 1.6 m up looking down -Z, eyes 0.4 m apart (wide, so the shift is many
        // pixels), 90 degrees of view each way.
        SimulatedXrOptions simulated;
        simulated.viewWidth = kSize;
        simulated.viewHeight = kSize;
        simulated.interpupillaryDistance = 0.4f;
        const auto backendXr = std::make_shared<SimulatedXrBackend>(simulated);
        const auto xr = std::make_shared<XrManager>(backendXr);

        auto engine = makeTestEngine<RenderComponentSystem, CameraComponentSystem>(device,
            [&](AppOptions& options) { options.xr = xr; });
        check(engine->xr() == xr, "the engine holds the XR manager it was given");

        const auto red = emissive(Color(1.0f, 0.0f, 0.0f, 1.0f));
        const auto green = emissive(Color(0.0f, 1.0f, 0.0f, 1.0f));
        // 2 m ahead of the head, 0.2 m above it.
        addBox(engine.get(), Vector3(0.0f, 1.8f, -2.0f), 0.3f, red.get());

        auto* rig = addTo(engine->root(), newEntity(engine.get(), "rig"));
        auto* cameraEntity = addTo(rig, newEntity(engine.get(), "camera"));
        auto* cameraComponent = static_cast<CameraComponent*>(cameraEntity->addComponent<CameraComponent>());
        Camera* camera = cameraComponent->camera();
        camera->setClearColor(Color(0.0f, 0.0f, 1.0f, 1.0f));
        camera->setNearClip(0.1f);
        camera->setFarClip(100.0f);
        camera->setFov(33.0f);
        cameraEntity->setLocalPosition(5.0f, 5.0f, 5.0f);

        int prerenders = 0;
        engine->scene()->on("prerender", [&](CameraComponent* which) {
            prerenders += which == cameraComponent ? 1 : 0;
        });

        engine->start();

        int started = 0;
        int ended = 0;
        xr->on("start", [&] { ++started; });
        xr->on("end", [&] { ++ended; });

        // ---- stereo ----
        XrSessionOptions sessionOptions;
        sessionOptions.samples = 1;
        check(xr->isAvailable(XrSessionType::Vr), "the simulated backend offers a VR session");
        check(xr->start(cameraComponent, sessionOptions) && xr->active() && started == 1, "the session starts");
        check(camera->multiView(), "the camera renders views while the session runs");

        XrInputSourceState hand;
        hand.id = 7;
        hand.handedness = XrHandedness::Right;
        hand.gripValid = true;
        hand.grip.position = Vector3(0.2f, 1.2f, -0.3f);
        hand.rayValid = true;
        hand.ray = hand.grip;
        backendXr->setInputSource(hand);
        int added = 0;
        int selectStarts = 0;
        int selects = 0;
        xr->input()->on("add", [&](XrInputSource*) { ++added; });
        xr->input()->on("selectstart", [&](XrInputSource*) { ++selectStarts; });
        xr->input()->on("select", [&](XrInputSource*) { ++selects; });

        prerenders = 0;
        for (int frame = 0; frame < 3; ++frame) {
            hand.select = frame == 1;
            backendXr->setInputSource(hand);
            engine->waitForNextFrame();
            engine->update(1.0f / 60.0f);
            engine->render();
        }
        check(prerenders == 3, "the camera's prerender fires once a frame: " + std::to_string(prerenders) + " in 3");
        check(backendXr->framesBegun() == 3 && backendXr->framesEnded() == 3, "every frame begun is ended");
        check(backendXr->lastSubmission().rendered && backendXr->lastSubmission().views.size() == 2,
            "the frame submitted carries both views");
        check(xr->views().size() == 2 && xr->view(XrEye::Left) && xr->view(XrEye::Right), "two views, one per eye");
        check(near(cameraEntity->localPosition().getY(), 1.6f, 1e-5f) && near(cameraEntity->localPosition().getX(), 0.0f, 1e-5f),
            "the camera follows the head");
        check(near(camera->fov(), 90.0f, 1e-3f) && near(camera->aspectRatio(), 1.0f, 1e-5f),
            "the camera's own projection follows the first view's");
        check(added == 1 && selectStarts == 1 && selects == 1, "an input source is added and its press fires once");
        const XrInputSource* source = xr->input()->find(XrHandedness::Right);
        check(source && source->grip() && source->getPosition() && near(source->getPosition()->getY(), 1.2f, 1e-5f),
            "the input source reports its pose");

        // x: the box is 0.2 m to the right of the left eye at 2 m, tan(45) = 1: NDC 0.1.
        // y: 0.2 m above at 2 m: NDC 0.1, and row 0 is the top.
        const Blob left = findBlob(backendXr->viewTexture(0, 0), 0);
        const Blob right = findBlob(backendXr->viewTexture(1, 0), 0);
        const double half = kSize * 0.5;
        check(left.count > 20 && right.count > 20, "both eyes see the box: " + std::to_string(left.count) + ", "
            + std::to_string(right.count) + " pixels");
        check(std::abs(left.x - half * 1.1) < 1.0, "the left eye sees it right of centre: x = " + std::to_string(left.x));
        check(std::abs(right.x - half * 0.9) < 1.0, "the right eye sees it left of centre: x = " + std::to_string(right.x));
        check(std::abs(left.y - half * 0.9) < 1.0 && std::abs(right.y - half * 0.9) < 1.0,
            "and both above centre: y = " + std::to_string(left.y) + ", " + std::to_string(right.y));
        const auto clear = cornerPixel(backendXr->viewTexture(0, 0));
        check(clear[2] > 200 && clear[0] < 30, "a view is cleared to the camera's colour");

        // The reference space is the camera's parent: moving the rig moves the views.
        rig->setLocalPosition(0.2f, 0.0f, 0.0f);
        engine->waitForNextFrame();
        engine->update(1.0f / 60.0f);
        engine->render();
        const Blob moved = findBlob(backendXr->viewTexture(0, 0), 0);
        check(std::abs(moved.x - half) < 1.0, "moving the camera's parent moves the views: x = " + std::to_string(moved.x));
        rig->setLocalPosition(0.0f, 0.0f, 0.0f);

        xr->end();
        check(!xr->active() && ended == 1 && !camera->multiView(), "the session ends");
        check(near(camera->fov(), 33.0f, 1e-5f) && near(cameraEntity->localPosition().getX(), 5.0f, 1e-5f) &&
            camera->aspectRatioMode() == AspectRatioMode::ASPECT_AUTO, "and the camera has its own settings back");
        engine->update(1.0f / 60.0f);
        engine->render();

        // ---- depth slices ----
        addBox(engine.get(), Vector3(-0.3f, 1.6f, -1.0f), 0.2f, green.get());
        sessionOptions.depthSlices = 2;
        sessionOptions.depthSliceBoundaries = {0.1f, 1.5f, 100.0f};
        check(xr->start(cameraComponent, sessionOptions), "a session with two depth slices starts");
        for (int frame = 0; frame < 2; ++frame) {
            engine->waitForNextFrame();
            engine->update(1.0f / 60.0f);
            engine->render();
        }
        check(camera->views().size() == 4, "two eyes of two slices are four views");
        Texture* nearSlice = backendXr->viewTexture(0, 0);
        Texture* farSlice = backendXr->viewTexture(0, 1);
        check(findBlob(nearSlice, 1).count > 20 && findBlob(nearSlice, 0).count == 0,
            "the near slice holds the near box and not the far one");
        check(findBlob(farSlice, 0).count > 20 && findBlob(farSlice, 1).count == 0,
            "the far slice holds the far box and not the near one");
        const auto nearClear = cornerPixel(nearSlice);
        const auto farClear = cornerPixel(farSlice);
        check(nearClear[3] == 0 && nearClear[2] == 0, "the near slice is cleared to transparent black");
        check(farClear[2] > 200 && farClear[3] == 255, "the far slice to the camera's colour");
        check(backendXr->lastSubmission().depthSlices == 2 && backendXr->lastSubmission().depthSliceBoundaries.size() == 3,
            "the submission names the slices");

        // A runtime that loses the session ends it.
        backendXr->loseSession();
        engine->update(1.0f / 60.0f);
        check(!xr->active() && !camera->multiView(), "a lost session ends");
        engine->render();

#ifdef VISUTWIN_XR_TEST_RUNTIME_IMAGES
        // ---- images owned by a runtime (the OpenXR Vulkan binding's path) ----
        if (auto* vulkan = dynamic_cast<VulkanGraphicsDevice*>(device.get())) {
            RuntimeImage runtimeImages[2];
            std::vector<OpenXrSwapchainImage> wrapped;
            const auto graphics = createOpenXrVulkanGraphics(*device);
            bool made = graphics != nullptr;
            for (auto& runtimeImage : runtimeImages) {
                made = made && runtimeImage.create(*vulkan, kSize);
                if (made) {
                    wrapped.push_back(wrapOpenXrVulkanImage(*device, runtimeImage.image, VK_FORMAT_R8G8B8A8_SRGB,
                        kSize, kSize, 4, "RuntimeImage"));
                    made = wrapped.back().target != nullptr;
                }
            }
            check(made, "runtime-owned sRGB images are wrapped in engine targets");
            if (made) {
                auto* hardware = dynamic_cast<gpu::VulkanTexture*>(wrapped[0].texture->impl());
                check(hardware && hardware->externalImage() && hardware->image() == runtimeImages[0].image &&
                    hardware->format() == VK_FORMAT_R8G8B8A8_UNORM, "each drawn through a linear view of the runtime's image");

                const auto runtimeBackend = std::make_shared<RuntimeImageBackend>(simulated, graphics.get(), &wrapped);
                xr->setBackend(runtimeBackend);
                XrSessionOptions runtimeOptions;
                check(xr->start(cameraComponent, runtimeOptions), "a session starts over the runtime's images");
                for (int frame = 0; frame < 3; ++frame) {
                    engine->waitForNextFrame();
                    engine->update(1.0f / 60.0f);
                    engine->render();
                }
                check(runtimeBackend->finished == 3 && runtimeBackend->releasedAsAttachment,
                    "every frame hands the images back as colour attachments");
                const Blob runtimeLeft = findBlob(wrapped[0].texture.get(), 0);
                const Blob runtimeRight = findBlob(wrapped[1].texture.get(), 0);
                check(runtimeLeft.count > 20 && std::abs(runtimeLeft.x - half * 1.1) < 1.0 &&
                    std::abs(runtimeRight.x - half * 0.9) < 1.0 && std::abs(runtimeLeft.y - half * 0.9) < 1.0,
                    "the eyes are drawn into them: x = " + std::to_string(runtimeLeft.x) + ", " + std::to_string(runtimeRight.x));
                // Pure red and the blue clear, as the forward pass encodes them: stored as written.
                std::vector<uint8_t> centre;
                const bool read = wrapped[0].texture->read(centre, static_cast<uint32_t>(runtimeLeft.x),
                    static_cast<uint32_t>(runtimeLeft.y), 1, 1);
                const auto runtimeClear = cornerPixel(wrapped[0].texture.get());
                check(read && centre.size() >= 4 && centre[0] == 255 && centre[1] == 0 && runtimeClear[2] == 255 &&
                    runtimeClear[0] == 0, "and the gamma-encoded bytes are stored as written, not encoded again");
                xr->end();
                engine->update(1.0f / 60.0f);
                engine->render();
            }
            xr->setBackend(nullptr);
            // The wrappers before the images, as the binding lets go of them before its swapchain.
            if (graphics) {
                graphics->releaseImages(wrapped);
            }
            wrapped.clear();
            engine->update(1.0f / 60.0f);
            engine->render();
            vkDeviceWaitIdle(vulkan->device());
            for (auto& runtimeImage : runtimeImages) {
                runtimeImage.destroy();
            }
        }
#endif

        engine->destroy();
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return finish("xr");
}
