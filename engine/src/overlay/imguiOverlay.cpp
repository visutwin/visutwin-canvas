//
// ImGui overlay for VisuTwin Canvas — implementation.
//
// One overlay, two renderer backends. init() picks the ImGui backend from the
// device it is handed; every stage after that dispatches on _renderer, because
// new-frame, draw submission and shutdown are different calls in each.
//
// The two reach the back buffer deliberately differently:
//   Metal  — makes and commits its own command buffer against the frame drawable.
//            Its startRenderPass already makes a command buffer per pass, so one
//            more costs nothing and needs no cooperation from the device.
//   Vulkan — records into the frame's command buffer, which is STILL OPEN at the
//            "postrender" hook, through VulkanGraphicsDevice::beginOverlayRendering().
//            A command buffer of its own could not work: the swapchain image is
//            transitioned to PRESENT_SRC by frameEnd on that same buffer, so a
//            separately submitted overlay would race the present it belongs to.
//
#include "imguiOverlay.h"

#include <cmath>

#if defined(VISUTWIN_HAS_METAL)
// metal-cpp headers must come BEFORE imgui_impl_metal.h so that
// IMGUI_IMPL_METAL_CPP picks up the MTL:: types.
#include <Metal/Metal.hpp>
#include <QuartzCore/CAMetalDrawable.hpp>
#define IMGUI_IMPL_METAL_CPP
#endif

#include <imgui.h>
#include <implot.h>
#include <imgui_impl_sdl3.h>

#if defined(VISUTWIN_HAS_METAL)
#include <imgui_impl_metal.h>
#include "platform/graphics/metal/metalGraphicsDevice.h"
#endif

#if defined(VISUTWIN_HAS_VULKAN)
#include <imgui_impl_vulkan.h>
#include "platform/graphics/vulkan/vulkanGraphicsDevice.h"
#endif

#include <SDL3/SDL.h>

#include "spdlog/spdlog.h"

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
#if defined(VISUTWIN_HAS_VULKAN)
    namespace
    {
        // ImGui keeps the InitInfo's pColorAttachmentFormats POINTER and reads it
        // when it builds its pipeline, so the format cannot be a local in init().
        // There is one overlay per process, so one slot is enough.
        VkFormat gOverlayColorFormat = VK_FORMAT_UNDEFINED;
    }
#endif

    // ── Lifecycle ────────────────────────────────────────────────────────

    ImGuiOverlay::~ImGuiOverlay()
    {
        if (_initialized) {
            shutdown();
        }
    }

    ImGuiOverlay::ImGuiOverlay(ImGuiOverlay&& other) noexcept
        : _renderer(other._renderer)
        , _device(other._device)
        , _window(other._window)
        , _initialized(other._initialized)
        , _vulkanDescriptorPool(other._vulkanDescriptorPool)
        , _viewProjection(other._viewProjection)
        , _windowW(other._windowW)
        , _windowH(other._windowH)
    {
        other._initialized = false;
        other._renderer = Renderer::None;
        other._device = nullptr;
        other._window = nullptr;
        other._vulkanDescriptorPool = 0;
    }

    ImGuiOverlay& ImGuiOverlay::operator=(ImGuiOverlay&& other) noexcept
    {
        if (this != &other) {
            if (_initialized) shutdown();
            _renderer = other._renderer;
            _device = other._device;
            _window = other._window;
            _initialized = other._initialized;
            _vulkanDescriptorPool = other._vulkanDescriptorPool;
            _viewProjection = other._viewProjection;
            _windowW = other._windowW;
            _windowH = other._windowH;
            other._initialized = false;
            other._renderer = Renderer::None;
            other._device = nullptr;
            other._window = nullptr;
            other._vulkanDescriptorPool = 0;
        }
        return *this;
    }

    void ImGuiOverlay::init(GraphicsDevice* device, SDL_Window* window)
    {
        if (_initialized) {
            spdlog::warn("ImGuiOverlay::init called on already-initialized overlay");
            return;
        }
        if (!device || !window) {
            spdlog::warn("ImGuiOverlay::init needs both a device and a window");
            return;
        }

        // Decide the backend BEFORE creating any ImGui state, so a device this
        // build cannot draw with leaves nothing behind to tear down.
        Renderer renderer = Renderer::None;
#if defined(VISUTWIN_HAS_METAL)
        auto* metalDevice = dynamic_cast<MetalGraphicsDevice*>(device);
        if (metalDevice) {
            renderer = Renderer::Metal;
        }
#endif
#if defined(VISUTWIN_HAS_VULKAN)
        auto* vulkanDevice = dynamic_cast<VulkanGraphicsDevice*>(device);
        if (vulkanDevice) {
            renderer = Renderer::Vulkan;
        }
#endif
        if (renderer == Renderer::None) {
            spdlog::warn("ImGuiOverlay: no ImGui renderer backend for this graphics device; "
                         "the overlay stays disabled");
            return;
        }

        _device = device;
        _window = window;
        _renderer = renderer;

        // Create ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        switch (_renderer) {
        case Renderer::Metal: {
#if defined(VISUTWIN_HAS_METAL)
            // Initialize backends — void* API (C++ mode, no __OBJC__)
            ImGui_ImplSDL3_InitForMetal(_window);
            ImGui_ImplMetal_Init(metalDevice->raw());  // MTL::Device* → void*
#endif
            break;
        }
        case Renderer::Vulkan: {
#if defined(VISUTWIN_HAS_VULKAN)
            // ImGui allocates its font texture and per-frame image descriptors from
            // its own pool. A pool of our own rather than the device's: ImGui frees
            // individual sets, which needs FREE_DESCRIPTOR_SET, and the engine's
            // frame pools are reset wholesale every frame.
            VkDescriptorPoolSize poolSize{};
            poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            poolSize.descriptorCount = 16;

            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
            poolInfo.maxSets = 16;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;

            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(vulkanDevice->device(), &poolInfo, nullptr, &pool)
                != VK_SUCCESS) {
                spdlog::error("ImGuiOverlay: could not create the Vulkan descriptor pool; "
                              "the overlay stays disabled");
                ImPlot::DestroyContext();
                ImGui::DestroyContext();
                _device = nullptr;
                _window = nullptr;
                _renderer = Renderer::None;
                return;
            }
            _vulkanDescriptorPool = reinterpret_cast<uint64_t>(pool);

            gOverlayColorFormat = vulkanDevice->swapchainFormat();

            ImGui_ImplSDL3_InitForVulkan(_window);

            ImGui_ImplVulkan_InitInfo initInfo{};
            initInfo.Instance = vulkanDevice->instance();
            initInfo.PhysicalDevice = vulkanDevice->physicalDevice();
            initInfo.Device = vulkanDevice->device();
            initInfo.QueueFamily = vulkanDevice->graphicsQueueFamily();
            initInfo.Queue = vulkanDevice->graphicsQueue();
            initInfo.DescriptorPool = pool;
            initInfo.MinImageCount = 2;
            initInfo.ImageCount = vulkanDevice->swapchainImageCount();
            initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

            // This backend renders with dynamic rendering, as the rest of the engine
            // does, so ImGui gets no VkRenderPass and builds its pipeline against the
            // swapchain's colour format alone. That is also why a swapchain resize
            // needs nothing here: ImGui owns no framebuffers or image views.
            initInfo.UseDynamicRendering = true;
            initInfo.PipelineRenderingCreateInfo = {
                VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
            initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &gOverlayColorFormat;

            ImGui_ImplVulkan_Init(&initInfo);
#endif
            break;
        }
        case Renderer::None:
            break;
        }

        // Apply digital twin theme
        applyDigitalTwinTheme();

        _initialized = true;
        spdlog::info("ImGuiOverlay initialized (ImGui {}, ImPlot, {} backend)", IMGUI_VERSION,
            _renderer == Renderer::Vulkan ? "Vulkan" : "Metal");
    }

    bool ImGuiOverlay::processEvent(const SDL_Event& event)
    {
        if (!_initialized) return false;
        return ImGui_ImplSDL3_ProcessEvent(&event);
    }

    bool ImGuiOverlay::wantCaptureMouse() const
    {
        if (!_initialized) return false;
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool ImGuiOverlay::wantCaptureKeyboard() const
    {
        if (!_initialized) return false;
        return ImGui::GetIO().WantCaptureKeyboard;
    }

    void ImGuiOverlay::beginFrame()
    {
        if (!_initialized || !_device) return;

        switch (_renderer) {
        case Renderer::Metal: {
#if defined(VISUTWIN_HAS_METAL)
            auto* metalDevice = static_cast<MetalGraphicsDevice*>(_device);
            auto* drawable = metalDevice->frameDrawable();
            if (!drawable) return;

            // Create a render pass descriptor targeting the drawable's texture.
            // LoadAction::Load preserves the 3D scene rendered underneath.
            auto* desc = MTL::RenderPassDescriptor::alloc()->init();
            auto* ca = desc->colorAttachments()->object(0);
            ca->setTexture(drawable->texture());
            ca->setLoadAction(MTL::LoadActionLoad);
            ca->setStoreAction(MTL::StoreActionStore);

            ImGui_ImplMetal_NewFrame(desc);     // void*
            desc->release();
#endif
            break;
        }
        case Renderer::Vulkan: {
#if defined(VISUTWIN_HAS_VULKAN)
            // Nothing to hand it: the pipeline is already built against the
            // swapchain format, and the target is supplied at draw time.
            ImGui_ImplVulkan_NewFrame();
#endif
            break;
        }
        case Renderer::None:
            return;
        }

        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiOverlay::endFrame()
    {
        if (!_initialized) return;
        ImGui::Render();
    }

    void ImGuiOverlay::renderToGPU()
    {
        if (!_initialized || !_device) return;

        auto* drawData = ImGui::GetDrawData();
        if (!drawData) return;

        switch (_renderer) {
        case Renderer::Metal: {
#if defined(VISUTWIN_HAS_METAL)
            auto* metalDevice = static_cast<MetalGraphicsDevice*>(_device);
            auto* drawable = metalDevice->frameDrawable();
            if (!drawable) return;

            // Create a fresh render pass descriptor for encoding.
            auto* desc = MTL::RenderPassDescriptor::alloc()->init();
            auto* ca = desc->colorAttachments()->object(0);
            ca->setTexture(drawable->texture());
            ca->setLoadAction(MTL::LoadActionLoad);
            ca->setStoreAction(MTL::StoreActionStore);

            auto* cmdBuf = metalDevice->commandQueue()->commandBuffer();
            if (!cmdBuf) {
                desc->release();
                return;
            }

            auto* encoder = cmdBuf->renderCommandEncoder(desc);
            if (!encoder) {
                desc->release();
                return;
            }

            // Encode ImGui draw data — void* API bridges metal-cpp ↔ Obj-C
            ImGui_ImplMetal_RenderDrawData(drawData, cmdBuf, encoder);

            encoder->endEncoding();
            cmdBuf->commit();

            desc->release();
#endif
            break;
        }
        case Renderer::Vulkan: {
#if defined(VISUTWIN_HAS_VULKAN)
            auto* vulkanDevice = static_cast<VulkanGraphicsDevice*>(_device);
            // The device owns the swapchain image's layout, so it opens and closes
            // the pass; a false here means there is no frame to draw into, and
            // endOverlayRendering must not follow it.
            if (!vulkanDevice->beginOverlayRendering()) return;
            ImGui_ImplVulkan_RenderDrawData(drawData, vulkanDevice->currentCommandBuffer());
            vulkanDevice->endOverlayRendering();
#endif
            break;
        }
        case Renderer::None:
            break;
        }
    }

    void ImGuiOverlay::shutdown()
    {
        if (!_initialized) return;

        switch (_renderer) {
        case Renderer::Metal:
#if defined(VISUTWIN_HAS_METAL)
            ImGui_ImplMetal_Shutdown();
#endif
            break;
        case Renderer::Vulkan: {
#if defined(VISUTWIN_HAS_VULKAN)
            auto* vulkanDevice = static_cast<VulkanGraphicsDevice*>(_device);
            // ImGui's font texture and pipeline are still in flight if a frame is
            // outstanding, and its shutdown does not wait. The device is about to be
            // destroyed anyway, so idle first rather than free from under the GPU.
            if (vulkanDevice && vulkanDevice->device() != VK_NULL_HANDLE) {
                vkDeviceWaitIdle(vulkanDevice->device());
            }
            ImGui_ImplVulkan_Shutdown();
            if (_vulkanDescriptorPool != 0 && vulkanDevice) {
                vkDestroyDescriptorPool(vulkanDevice->device(),
                    reinterpret_cast<VkDescriptorPool>(_vulkanDescriptorPool), nullptr);
            }
            _vulkanDescriptorPool = 0;
            gOverlayColorFormat = VK_FORMAT_UNDEFINED;
#endif
            break;
        }
        case Renderer::None:
            break;
        }

        ImGui_ImplSDL3_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();

        _initialized = false;
        _renderer = Renderer::None;
        _device = nullptr;
        _window = nullptr;

        spdlog::info("ImGuiOverlay shut down");
    }

    // ── 3D-anchored labels ──────────────────────────────────────────────

    bool ImGuiOverlay::worldToScreen(const Vector3& worldPos, float& screenX, float& screenY) const
    {
        // Transform to clip space
        Vector4 clip = _viewProjection * Vector4(worldPos.getX(), worldPos.getY(), worldPos.getZ(), 1.0f);

        // Behind camera check
        if (clip.getW() <= 0.0f) return false;

        // NDC
        const float ndcX = clip.getX() / clip.getW();
        const float ndcY = clip.getY() / clip.getW();

        // NDC to screen: X [-1,+1] → [0, windowW], Y [-1,+1] → [windowH, 0]
        screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(_windowW);
        screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(_windowH);

        return true;
    }

    void ImGuiOverlay::label3D(const Vector3& worldPos, const char* text, const Color& color)
    {
        float sx, sy;
        if (!worldToScreen(worldPos, sx, sy)) return;

        // Render as a foreground overlay text (no window chrome)
        auto* drawList = ImGui::GetForegroundDrawList();
        const ImU32 imColor = IM_COL32(
            static_cast<int>(color.r * 255.0f),
            static_cast<int>(color.g * 255.0f),
            static_cast<int>(color.b * 255.0f),
            static_cast<int>(color.a * 255.0f)
        );
        drawList->AddText(ImVec2(sx, sy), imColor, text);
    }

    void ImGuiOverlay::panelLabel3D(const Vector3& worldPos, const char* title, const char* body,
                                     const Color& panelColor)
    {
        float sx, sy;
        if (!worldToScreen(worldPos, sx, sy)) return;

        // Create a small ImGui window at the projected screen position
        ImGui::SetNextWindowPos(ImVec2(sx + 12.0f, sy - 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(panelColor.a);

        const std::string windowId = std::string("##label3d_") + title;
        ImGui::Begin(windowId.c_str(), nullptr,
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNav);

        ImGui::TextColored(ImVec4(0.22f, 0.74f, 0.97f, 1.0f), "%s", title);
        if (body && body[0] != '\0') {
            ImGui::TextColored(ImVec4(0.58f, 0.64f, 0.72f, 0.80f), "%s", body);
        }

        ImGui::End();
    }

    // ── Theme ────────────────────────────────────────────────────────────

    void ImGuiOverlay::applyDigitalTwinTheme()
    {
        ImGuiStyle& style = ImGui::GetStyle();

        // Panel geometry
        style.WindowRounding    = 8.0f;
        style.FrameRounding     = 4.0f;
        style.PopupRounding     = 6.0f;
        style.ScrollbarRounding = 4.0f;
        style.GrabRounding      = 3.0f;
        style.TabRounding       = 4.0f;
        style.ChildRounding     = 6.0f;

        style.WindowPadding     = ImVec2(12.0f, 10.0f);
        style.FramePadding      = ImVec2(8.0f, 4.0f);
        style.ItemSpacing       = ImVec2(8.0f, 6.0f);
        style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
        style.ScrollbarSize     = 10.0f;
        style.GrabMinSize       = 8.0f;

        style.WindowBorderSize  = 1.0f;
        style.FrameBorderSize   = 0.0f;
        style.PopupBorderSize   = 1.0f;

        // ── Digital twin dark glassmorphism palette ───────────────────
        auto* colors = style.Colors;

        // Window
        colors[ImGuiCol_WindowBg]           = ImVec4(0.059f, 0.090f, 0.165f, 0.88f);
        colors[ImGuiCol_ChildBg]            = ImVec4(0.047f, 0.071f, 0.133f, 0.60f);
        colors[ImGuiCol_PopupBg]            = ImVec4(0.059f, 0.090f, 0.165f, 0.94f);

        // Borders
        colors[ImGuiCol_Border]             = ImVec4(0.278f, 0.333f, 0.412f, 0.40f);
        colors[ImGuiCol_BorderShadow]       = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

        // Text
        colors[ImGuiCol_Text]               = ImVec4(0.945f, 0.961f, 0.976f, 0.95f);
        colors[ImGuiCol_TextDisabled]       = ImVec4(0.580f, 0.639f, 0.722f, 0.50f);

        // Headers
        colors[ImGuiCol_Header]             = ImVec4(0.118f, 0.161f, 0.231f, 0.80f);
        colors[ImGuiCol_HeaderHovered]      = ImVec4(0.220f, 0.741f, 0.973f, 0.30f);
        colors[ImGuiCol_HeaderActive]       = ImVec4(0.220f, 0.741f, 0.973f, 0.45f);

        // Buttons
        colors[ImGuiCol_Button]             = ImVec4(0.118f, 0.161f, 0.231f, 0.80f);
        colors[ImGuiCol_ButtonHovered]      = ImVec4(0.220f, 0.741f, 0.973f, 0.40f);
        colors[ImGuiCol_ButtonActive]       = ImVec4(0.220f, 0.741f, 0.973f, 0.65f);

        // Frame
        colors[ImGuiCol_FrameBg]            = ImVec4(0.078f, 0.110f, 0.180f, 0.80f);
        colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.220f, 0.741f, 0.973f, 0.20f);
        colors[ImGuiCol_FrameBgActive]      = ImVec4(0.220f, 0.741f, 0.973f, 0.35f);

        // Title bar
        colors[ImGuiCol_TitleBg]            = ImVec4(0.047f, 0.071f, 0.133f, 0.95f);
        colors[ImGuiCol_TitleBgActive]      = ImVec4(0.059f, 0.090f, 0.165f, 0.95f);
        colors[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.047f, 0.071f, 0.133f, 0.70f);

        // Tabs
        colors[ImGuiCol_Tab]               = ImVec4(0.078f, 0.110f, 0.180f, 0.80f);
        colors[ImGuiCol_TabHovered]        = ImVec4(0.220f, 0.741f, 0.973f, 0.40f);
        colors[ImGuiCol_TabSelected]       = ImVec4(0.220f, 0.741f, 0.973f, 0.25f);
        colors[ImGuiCol_TabDimmed]         = ImVec4(0.059f, 0.090f, 0.165f, 0.80f);
        colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.118f, 0.161f, 0.231f, 0.90f);

        // Scrollbar
        colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.047f, 0.071f, 0.133f, 0.40f);
        colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.278f, 0.333f, 0.412f, 0.50f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.220f, 0.741f, 0.973f, 0.40f);
        colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.220f, 0.741f, 0.973f, 0.60f);

        // Slider / grab
        colors[ImGuiCol_SliderGrab]         = ImVec4(0.220f, 0.741f, 0.973f, 0.70f);
        colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.404f, 0.910f, 0.976f, 0.90f);
        colors[ImGuiCol_CheckMark]          = ImVec4(0.220f, 0.741f, 0.973f, 0.90f);

        // Separator
        colors[ImGuiCol_Separator]          = ImVec4(0.278f, 0.333f, 0.412f, 0.40f);
        colors[ImGuiCol_SeparatorHovered]   = ImVec4(0.220f, 0.741f, 0.973f, 0.40f);
        colors[ImGuiCol_SeparatorActive]    = ImVec4(0.220f, 0.741f, 0.973f, 0.65f);

        // Resize grip
        colors[ImGuiCol_ResizeGrip]         = ImVec4(0.220f, 0.741f, 0.973f, 0.15f);
        colors[ImGuiCol_ResizeGripHovered]  = ImVec4(0.220f, 0.741f, 0.973f, 0.40f);
        colors[ImGuiCol_ResizeGripActive]   = ImVec4(0.220f, 0.741f, 0.973f, 0.65f);

        // Plot
        colors[ImGuiCol_PlotLines]          = ImVec4(0.220f, 0.741f, 0.973f, 0.80f);
        colors[ImGuiCol_PlotLinesHovered]   = ImVec4(0.404f, 0.910f, 0.976f, 1.00f);
        colors[ImGuiCol_PlotHistogram]      = ImVec4(0.220f, 0.741f, 0.973f, 0.70f);
        colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.404f, 0.910f, 0.976f, 1.00f);

        // Table
        colors[ImGuiCol_TableHeaderBg]      = ImVec4(0.078f, 0.110f, 0.180f, 0.90f);
        colors[ImGuiCol_TableBorderStrong]  = ImVec4(0.278f, 0.333f, 0.412f, 0.50f);
        colors[ImGuiCol_TableBorderLight]   = ImVec4(0.278f, 0.333f, 0.412f, 0.25f);
        colors[ImGuiCol_TableRowBg]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colors[ImGuiCol_TableRowBgAlt]      = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);

        // Misc
        colors[ImGuiCol_TextSelectedBg]     = ImVec4(0.220f, 0.741f, 0.973f, 0.25f);
        colors[ImGuiCol_DragDropTarget]     = ImVec4(0.220f, 0.741f, 0.973f, 0.70f);
        colors[ImGuiCol_NavHighlight]       = ImVec4(0.220f, 0.741f, 0.973f, 0.70f);
        colors[ImGuiCol_ModalWindowDimBg]   = ImVec4(0.0f, 0.0f, 0.0f, 0.50f);

        // ── ImPlot theme ─────────────────────────────────────────────
        ImPlot::StyleColorsAuto();
        ImPlotStyle& plotStyle = ImPlot::GetStyle();
        plotStyle.Colors[ImPlotCol_PlotBorder]  = ImVec4(0.278f, 0.333f, 0.412f, 0.40f);
        plotStyle.Colors[ImPlotCol_PlotBg]      = ImVec4(0.047f, 0.071f, 0.133f, 0.60f);
        plotStyle.Colors[ImPlotCol_LegendBg]    = ImVec4(0.059f, 0.090f, 0.165f, 0.85f);
        plotStyle.Colors[ImPlotCol_LegendBorder]= ImVec4(0.278f, 0.333f, 0.412f, 0.30f);
        plotStyle.Colors[ImPlotCol_LegendText]  = ImVec4(0.945f, 0.961f, 0.976f, 0.90f);

        // Custom colormap: digital twin palette (cyan → teal → emerald → amber → red)
        static const ImVec4 dtColors[] = {
            ImVec4(0.220f, 0.741f, 0.973f, 1.0f),  // cyan-400
            ImVec4(0.173f, 0.824f, 0.773f, 1.0f),  // teal-400
            ImVec4(0.204f, 0.827f, 0.600f, 1.0f),  // emerald-400
            ImVec4(0.984f, 0.749f, 0.141f, 1.0f),  // amber-400
            ImVec4(0.973f, 0.443f, 0.443f, 1.0f),  // red-400
            ImVec4(0.659f, 0.533f, 0.973f, 1.0f),  // violet-400
        };
        ImPlot::AddColormap("DigitalTwin", dtColors, 6);

        spdlog::info("Digital twin theme applied");
    }

} // namespace visutwin::canvas
