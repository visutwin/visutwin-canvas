//
// ImGui overlay for VisuTwin Canvas — digital twin HUD rendering.
//
// Provides GPU-accelerated UI overlays rendered on top of the 3D scene:
// - Floating data panels, sensor readouts, charts (via ImPlot)
// - 3D-anchored labels projected to screen space
// - Digital twin dark glassmorphism theme
//
// Integration point: hook into Engine's "postrender" event so the overlay
// renders after all 3D passes but before the frame is presented.
//
//
#pragma once

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
    class MetalGraphicsDevice;

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

        /// Initialize ImGui context, Metal backend, SDL3 backend, and theme.
        /// Call once after creating the graphics device and SDL window.
        void init(MetalGraphicsDevice* device, SDL_Window* window);

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

        /// Encode ImGui draw data into a Metal command buffer targeting the
        /// frame's drawable. Call between endFrame() and the present.
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

        MetalGraphicsDevice* _device = nullptr;
        SDL_Window* _window = nullptr;
        bool _initialized = false;

        // View-projection for 3D label projection
        Matrix4 _viewProjection;
        int _windowW = 1280;
        int _windowH = 900;
    };

} // namespace visutwin::canvas
