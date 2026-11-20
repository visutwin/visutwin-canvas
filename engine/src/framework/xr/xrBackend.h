// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "xrTypes.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    struct GraphicsDeviceOptions;

    /**
     * What drives an XR session: the runtime that tracks the head and the controllers, hands
     * out the images the views are drawn into, and shows them. The engine owns none: an
     * application gives the XrManager one (createOpenXrBackend for a headset, a
     * SimulatedXrBackend for development and tests).
     *
     * A frame is: waitFrame, locateViews and pollInput (the update), beginFrame (before the
     * frame is rendered), endFrame (after it is submitted to the GPU).
     */
    class XrBackend
    {
    public:
        virtual ~XrBackend() = default;

        [[nodiscard]] virtual const char* name() const = 0;

        /// Whether a session of the type could be started now.
        virtual bool isAvailable(XrSessionType type) = 0;

        /// What the backend needs of the graphics device a session will render through, to be
        /// called BEFORE the device is created: an OpenXR runtime names the Vulkan extensions
        /// its compositor needs and the GPU the headset is wired to. A device created without
        /// it may be refused by begin().
        virtual void configureGraphicsDevice(GraphicsDeviceOptions& /*options*/) {}

        /// Starts a session rendering through `device`. On failure returns false with the reason.
        virtual bool begin(const XrSessionOptions& options, GraphicsDevice& device, std::string& error) = 0;

        virtual void end() = 0;

        /// False once the runtime has ended or lost the session; the manager then ends it.
        [[nodiscard]] virtual bool running() const = 0;

        [[nodiscard]] virtual XrVisibility visibility() const { return XrVisibility::Visible; }

        /// The size in pixels of one view's target.
        [[nodiscard]] virtual std::pair<int, int> viewSize() const = 0;

        /// Blocks until the next frame should be prepared.
        virtual bool waitFrame(XrFrameTiming& timing) = 0;

        /// The views at the frame's display time, in the session's reference space. False
        /// when tracking has no pose to give; the frame then renders nothing.
        virtual bool locateViews(std::vector<XrViewState>& views) = 0;

        /// The input sources at the frame's display time.
        virtual void pollInput(std::vector<XrInputSourceState>& sources) = 0;

        /// Takes the images the frame is drawn into: `views x depthSlices` targets, the slices
        /// of a view together, nearest first. False when there is nothing to draw into; the
        /// frame is still ended.
        virtual bool beginFrame(std::vector<XrViewTarget>& targets) = 0;

        /// The frame is rendered and its GPU work not yet submitted: the last point to record
        /// anything for it (the images back into the layout their owner expects).
        virtual void finishFrame() {}

        /// Hands the rendered frame over for display. The GPU work is submitted by now.
        virtual void endFrame(const XrFrameSubmission& submission) = 0;

        /// The colour image of a view (and slice) as last rendered, where the backend keeps
        /// one the engine can sample: what the window mirror shows. May be null.
        [[nodiscard]] virtual Texture* viewTexture(int /*view*/, int /*slice*/) const { return nullptr; }

        /// Vibrates an input source: intensity in [0, 1], for a duration in seconds.
        virtual void pulse(uint32_t /*inputSourceId*/, float /*intensity*/, float /*seconds*/) {}
    };
}
