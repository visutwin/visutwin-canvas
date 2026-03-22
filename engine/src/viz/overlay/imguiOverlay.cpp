//
// ImGui overlay for VisuTwin Canvas — implementation.
//
// Uses ImGui's Metal C++ API (IMGUI_IMPL_METAL_CPP) which provides
// type-safe MTL::Device*, MTL::CommandBuffer* etc. signatures that
// work directly with metal-cpp, no Objective-C++ needed.
//
//

#include "imguiOverlay.h"

#include <cmath>

// metal-cpp headers must come BEFORE imgui_impl_metal.h so that
// IMGUI_IMPL_METAL_CPP picks up the MTL:: types.
#include <Metal/Metal.hpp>
#include <QuartzCore/CAMetalDrawable.hpp>

#define IMGUI_IMPL_METAL_CPP
#include <imgui.h>
#include <implot.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_metal.h>

#include <SDL3/SDL.h>

#include "spdlog/spdlog.h"

#include "platform/graphics/metal/metalGraphicsDevice.h"

namespace visutwin::canvas
{
    // ── Lifecycle ────────────────────────────────────────────────────────

    ImGuiOverlay::~ImGuiOverlay()
    {
        if (_initialized) {
            shutdown();
        }
    }

    ImGuiOverlay::ImGuiOverlay(ImGuiOverlay&& other) noexcept
        : _device(other._device)
        , _window(other._window)
        , _initialized(other._initialized)
        , _viewProjection(other._viewProjection)
        , _windowW(other._windowW)
        , _windowH(other._windowH)
    {
        other._initialized = false;
        other._device = nullptr;
        other._window = nullptr;
    }

    ImGuiOverlay& ImGuiOverlay::operator=(ImGuiOverlay&& other) noexcept
    {
        if (this != &other) {
            if (_initialized) shutdown();
            _device = other._device;
            _window = other._window;
            _initialized = other._initialized;
            _viewProjection = other._viewProjection;
            _windowW = other._windowW;
            _windowH = other._windowH;
            other._initialized = false;
            other._device = nullptr;
            other._window = nullptr;
        }
        return *this;
    }

    void ImGuiOverlay::init(MetalGraphicsDevice* device, SDL_Window* window)
    {
        if (_initialized) {
            spdlog::warn("ImGuiOverlay::init called on already-initialized overlay");
            return;
        }

        _device = device;
        _window = window;

        // Create ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        // Initialize backends — void* API (C++ mode, no __OBJC__)
        ImGui_ImplSDL3_InitForMetal(_window);
        ImGui_ImplMetal_Init(_device->raw());  // MTL::Device* → void*

        // Apply digital twin theme
        applyDigitalTwinTheme();

        _initialized = true;
        spdlog::info("ImGuiOverlay initialized (ImGui {}, ImPlot)", IMGUI_VERSION);
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

        auto* drawable = _device->frameDrawable();
        if (!drawable) return;

        // Create a render pass descriptor targeting the drawable's texture.
        // LoadAction::Load preserves the 3D scene rendered underneath.
        auto* desc = MTL::RenderPassDescriptor::alloc()->init();
        auto* ca = desc->colorAttachments()->object(0);
        ca->setTexture(drawable->texture());
        ca->setLoadAction(MTL::LoadActionLoad);
        ca->setStoreAction(MTL::StoreActionStore);

        ImGui_ImplMetal_NewFrame(desc);     // void*
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        desc->release();
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

        auto* drawable = _device->frameDrawable();
        if (!drawable) return;

        // Create a fresh render pass descriptor for encoding.
        auto* desc = MTL::RenderPassDescriptor::alloc()->init();
        auto* ca = desc->colorAttachments()->object(0);
        ca->setTexture(drawable->texture());
        ca->setLoadAction(MTL::LoadActionLoad);
        ca->setStoreAction(MTL::StoreActionStore);

        auto* cmdBuf = _device->commandQueue()->commandBuffer();
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
    }

    void ImGuiOverlay::shutdown()
    {
        if (!_initialized) return;

        ImGui_ImplMetal_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();

        _initialized = false;
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
