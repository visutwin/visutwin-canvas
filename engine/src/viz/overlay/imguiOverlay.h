//
// ImGui overlay for VisuTwin Canvas — digital twin HUD rendering.
//
// Provides GPU-accelerated UI overlays rendered on top of the 3D scene:
// - Floating data panels, sensor readouts, charts (via ImPlot)
// - 3D-anchored labels projected to screen space
// - Digital twin dark glassmorphism theme
//
// Integration point: hook into Engine's "postrender" event so the overlay
// renders after all 3D passes but before the frame is presented. That hook is
// the only correct place on either backend — it fires while the frame is still
// recording, and frameEnd() presents immediately after it.
//
// Both backends are supported. init() picks the ImGui renderer backend from the
// device it is handed, and a device whose backend was not compiled into this
// build leaves the overlay uninitialized rather than failing — every other entry
// point tolerates that and does nothing.
//
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector3.h"

union SDL_Event;
struct SDL_Window;

namespace visutwin::canvas
{
    class GraphicsDevice;

    // ── 3D-anchored label descriptor ─────────────────────────────────────

    struct Label3D
    {
        Vector3 worldPos;
        std::string text;
        Color color = Color(0.94f, 0.96f, 0.98f, 0.95f);  // slate-100
        float fontSize = 0.0f;  // 0 = default
    };

    // ── ImGui overlay lifecycle manager ──────────────────────────────────

    class ImGuiOverlay
    {
    public:
        ImGuiOverlay() = default;
        ~ImGuiOverlay();

        // Non-copyable, movable
        ImGuiOverlay(const ImGuiOverlay&) = delete;
        ImGuiOverlay& operator=(const ImGuiOverlay&) = delete;
        ImGuiOverlay(ImGuiOverlay&&) noexcept;
        ImGuiOverlay& operator=(ImGuiOverlay&&) noexcept;

        /// Initialize the ImGui context, the SDL3 platform backend, the renderer
        /// backend matching `device`, and the theme. Call once after creating the
        /// graphics device and SDL window. Leaves the overlay uninitialized (and
        /// logs) when the device's backend was not compiled in.
        void init(GraphicsDevice* device, SDL_Window* window);

        /// Forward an SDL event to ImGui for input handling.
        /// Call this inside the SDL_PollEvent loop, BEFORE your own event handlers.
        /// Returns true if ImGui consumed the event (you can skip your own handling).
        bool processEvent(const SDL_Event& event);

        /// True if ImGui wants exclusive mouse input (cursor is over an ImGui window).
        bool wantCaptureMouse() const;

        /// True if ImGui wants exclusive keyboard input (an ImGui text field is active).
        bool wantCaptureKeyboard() const;

        /// Begin a new ImGui frame. Call once per frame before building any UI.
        void beginFrame();

        /// Finalize the ImGui frame and generate draw lists.
        /// Call after all ImGui::Begin/End calls are done.
        void endFrame();

        /// Draw the frame's ImGui data over the rendered scene. Call between
        /// endFrame() and the present — in practice from the "postrender" hook.
        ///
        /// The two backends reach the back buffer differently and deliberately:
        /// Metal makes and commits a command buffer of its own against the frame's
        /// drawable, while Vulkan records into the frame's command buffer, which is
        /// still open, through VulkanGraphicsDevice::beginOverlayRendering().
        void renderToGPU();

        /// Shut down ImGui and release all resources.
        void shutdown();

        /// Whether the overlay has been initialized.
        bool isInitialized() const { return _initialized; }

        // ── 3D-anchored label helpers ────────────────────────────────────

        /// Set the current frame's view-projection matrix (needed for worldToScreen).
        void setViewProjection(const Matrix4& vp) { _viewProjection = vp; }

        /// Set the current window dimensions (needed for NDC→screen conversion).
        void setWindowSize(int width, int height) { _windowW = width; _windowH = height; }

        /// Render a text label anchored to a 3D world position.
        /// The label is projected to screen space using the current view-projection.
        /// Call between beginFrame() and endFrame().
        void label3D(const Vector3& worldPos, const char* text,
                     const Color& color = Color(0.94f, 0.96f, 0.98f, 0.95f));

        /// Render a text label with a background panel anchored to a 3D world position.
        void panelLabel3D(const Vector3& worldPos, const char* title, const char* body,
                          const Color& panelColor = Color(0.06f, 0.09f, 0.16f, 0.88f));

        // ── Theme ────────────────────────────────────────────────────────

        /// Apply the digital twin dark glassmorphism theme.
        static void applyDigitalTwinTheme();

    private:
        /// Project a world position to screen coordinates.
        /// Returns false if the point is behind the camera.
        bool worldToScreen(const Vector3& worldPos, float& screenX, float& screenY) const;

        /// Which ImGui renderer backend init() bound. Every stage — new frame,
        /// draw submission, shutdown — is a different call per backend, and this
        /// is what selects it. None means init() found no backend it could use,
        /// in which case _initialized stays false.
        enum class Renderer
        {
            None,
            Metal,
            Vulkan
        };

        Renderer _renderer = Renderer::None;

        GraphicsDevice* _device = nullptr;
        SDL_Window* _window = nullptr;
        bool _initialized = false;

        /// Vulkan only: the descriptor pool ImGui allocates its font texture and
        /// per-frame descriptors from, destroyed at shutdown. Held as the raw
        /// handle value rather than VkDescriptorPool so this header pulls in no
        /// Vulkan types — it is compiled into Metal-only builds too. Every Vulkan
        /// non-dispatchable handle is a uint64_t, so nothing is lost in the cast.
        uint64_t _vulkanDescriptorPool = 0;

        // View-projection for 3D label projection
        Matrix4 _viewProjection;
        int _windowW = 1280;
        int _windowH = 900;
    };

} // namespace visutwin::canvas
