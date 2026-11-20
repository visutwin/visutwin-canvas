// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#pragma once

#include <map>
#include <memory>
#include <vector>

#include "xrBackend.h"

namespace visutwin::canvas
{
    class Texture;

    /// How the simulated headset is built.
    struct SimulatedXrOptions
    {
        /// One view (a handheld) or two (the eyes of a headset).
        int viewCount = 2;
        int viewWidth = 1024;
        int viewHeight = 1024;
        /// Distance between the eyes, in metres.
        float interpupillaryDistance = 0.064f;
        /// Each eye's field of view; the left eye's is mirrored from it, so an asymmetric
        /// one (wider outward than inward, as headsets have) stays symmetric between the eyes.
        XrFov fov{-0.7854f, 0.7854f, 0.7854f, -0.7854f};
        /// Where the head starts, in the reference space (a standing user's eye height).
        XrPose headPose{Vector3(0.0f, 1.6f, 0.0f), Quaternion(0.0f, 0.0f, 0.0f, 1.0f)};
        bool vrAvailable = true;
        bool arAvailable = true;
    };

    /**
     * An XR backend with no headset behind it: the views are engine textures, and the head
     * and the controllers are wherever the application last put them. It runs the whole XR
     * path of the engine on any device, for development at a desk and for tests, and its
     * images (viewTexture) can be shown, read back or handed to a display of one's own.
     */
    class SimulatedXrBackend : public XrBackend
    {
    public:
        explicit SimulatedXrBackend(const SimulatedXrOptions& options = {}) : _options(options), _head(options.headPose) {}

        /// The head in the reference space; the eyes sit half the IPD to either side of it.
        void setHeadPose(const XrPose& pose) { _head = pose; }
        [[nodiscard]] const XrPose& headPose() const { return _head; }

        /// Puts a controller in the session, or updates it (by its id).
        void setInputSource(const XrInputSourceState& state) { _sources[state.id] = state; }
        void removeInputSource(const uint32_t id) { _sources.erase(id); }

        /// Whether tracking has a pose to give: false makes frames render nothing.
        void setTracking(const bool tracking) { _tracking = tracking; }
        void setVisibility(const XrVisibility visibility) { _visibility = visibility; }
        /// Ends the session from the runtime's side, as a headset taken off for good does.
        void loseSession() { _running = false; }

        // What the engine did with it, for a test to hold.
        [[nodiscard]] int framesBegun() const { return _framesBegun; }
        [[nodiscard]] int framesEnded() const { return _framesEnded; }
        [[nodiscard]] const XrFrameSubmission& lastSubmission() const { return _lastSubmission; }
        [[nodiscard]] const std::vector<std::pair<uint32_t, float>>& pulses() const { return _pulses; }

        [[nodiscard]] const char* name() const override { return "simulated"; }
        bool isAvailable(XrSessionType type) override;
        bool begin(const XrSessionOptions& options, GraphicsDevice& device, std::string& error) override;
        void end() override;
        [[nodiscard]] bool running() const override { return _running; }
        [[nodiscard]] XrVisibility visibility() const override { return _visibility; }
        [[nodiscard]] std::pair<int, int> viewSize() const override { return {_width, _height}; }
        bool waitFrame(XrFrameTiming& timing) override;
        bool locateViews(std::vector<XrViewState>& views) override;
        void pollInput(std::vector<XrInputSourceState>& sources) override;
        bool beginFrame(std::vector<XrViewTarget>& targets) override;
        void endFrame(const XrFrameSubmission& submission) override;
        [[nodiscard]] Texture* viewTexture(int view, int slice) const override;
        void pulse(uint32_t inputSourceId, float intensity, float seconds) override;

    private:
        struct Image
        {
            std::shared_ptr<Texture> color;
            std::shared_ptr<RenderTarget> target;
        };

        SimulatedXrOptions _options;
        XrPose _head;
        std::map<uint32_t, XrInputSourceState> _sources;
        bool _tracking = true;
        bool _running = false;
        XrVisibility _visibility = XrVisibility::Visible;

        int _width = 0;
        int _height = 0;
        int _slices = 1;
        std::vector<Image> _images;

        int _framesBegun = 0;
        int _framesEnded = 0;
        double _time = 0.0;
        XrFrameSubmission _lastSubmission;
        std::vector<std::pair<uint32_t, float>> _pulses;
    };
}
