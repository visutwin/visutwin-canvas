// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025
//
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/eventHandler.h"
#include "framework/destroyWatch.h"
#include "scene/constants.h"
#include "xrInput.h"
#include "xrTypes.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Engine;
    class RenderPass;
    class XrBackend;

    using XrErrorCallback = std::function<void(const std::string* err)>;

    using MakeTickCallback = std::function<void(double, void*)>;

    /// A view of the session's current frame: an eye of a headset, the one view of a handheld.
    struct XrView
    {
        XrEye eye = XrEye::None;
        XrPose pose;
        XrFov fov;
        /// Over the camera's whole depth range.
        Matrix4 projection = Matrix4::identity();
        /// In the session's reference space: world to view, and the view's pose.
        Matrix4 viewMat = Matrix4::identity();
        Matrix4 viewInvMat = Matrix4::identity();
        /// In the world, through the parent of the session's camera.
        Matrix4 viewOffMat = Matrix4::identity();
        Matrix4 viewInvOffMat = Matrix4::identity();
        /// x, y, width, height in pixels of the view's target.
        Vector4 viewport{0.0f, 0.0f, 1.0f, 1.0f};
    };

    /**
     * Runs an XR session on a camera: while one is active the camera follows the head and
     * renders each of the session's views (the eyes of a headset) into the images its backend
     * provides, and the session's input sources are tracked. The tracking, the images and the
     * display belong to an XrBackend the application supplies.
     *
     * The session's reference space is the PARENT of the camera's entity: move that parent to
     * move the user through the world.
     *
     * Events: "start", "end", "update" (each frame, after the views and input are current),
     * "error" (const std::string&), "visibility:change" (XrVisibility).
     *
     * DEVIATION: of upstream's XR features this has the session, its views and its input
     * sources. Hit testing, anchors, plane and mesh detection, image tracking, light
     * estimation, hand joints, camera colour and depth, and DOM overlay are not here.
     */
    class XrManager : public EventHandler
    {
    public:
        explicit XrManager(std::shared_ptr<XrBackend> backend = nullptr);
        ~XrManager() override;

        /// The engine whose frames drive the session. Engine::init sets it.
        void setEngine(Engine* engine) { _engine = engine; }

        void setBackend(std::shared_ptr<XrBackend> backend);
        [[nodiscard]] XrBackend* backend() const { return _backend.get(); }

        /// Whether there is a backend at all.
        [[nodiscard]] bool supported() const { return _backend != nullptr; }
        [[nodiscard]] bool isAvailable(XrSessionType type) const;

        /// Starts a session on `camera`. False, with "error" fired and the callback called
        /// with the reason, when it could not start.
        bool start(CameraComponent* camera, const XrSessionOptions& options = {},
            const XrErrorCallback& callback = nullptr);

        /// Ends the session and gives the camera back its own projection and transform.
        void end(const XrErrorCallback& callback = nullptr);

        [[nodiscard]] bool active() const { return _active; }
        [[nodiscard]] XrSessionType type() const { return _options.type; }
        [[nodiscard]] XrSpaceType spaceType() const { return _options.spaceType; }
        [[nodiscard]] const XrSessionOptions& options() const { return _options; }
        [[nodiscard]] CameraComponent* camera() const { return _camera; }
        [[nodiscard]] XrVisibility visibilityState() const { return _visibility; }

        [[nodiscard]] const std::vector<XrView>& views() const { return _views; }
        [[nodiscard]] const XrView* view(XrEye eye) const;

        [[nodiscard]] XrInput* input() const { return _input.get(); }

        /// The distances the camera's depth range is cut at this frame, nearest first
        /// (XrSessionOptions::depthSlices + 1 of them).
        [[nodiscard]] const std::vector<float>& depthSliceBoundaries() const { return _sliceBoundaries; }

        // The frame, as the engine drives it. waitFrame blocks until the runtime wants the next
        // frame prepared (Engine::waitForNextFrame; update() calls it when nothing has).
        // update() brings the views, the camera and the input up to date (Engine::update) and
        // answers whether the frame has a pose. beginRender() takes the frame's images and
        // gives the camera its views (Engine::render, before the frame starts); finishRender()
        // lets the backend record its last commands (after the frame is drawn, before its GPU
        // work is submitted); endRender() hands the frame to the runtime (after it is).
        void waitFrame();
        bool update(float dt);
        void beginRender();
        void finishRender();
        void endRender();

        // Kept for the engine's tick, which predates the session: nothing to schedule here.
        void* requestAnimationFrame(const MakeTickCallback& /*callback*/) { return nullptr; }

    private:
        void fail(const std::string& message, const XrErrorCallback& callback);
        void restoreCamera();

        Engine* _engine = nullptr;
        std::shared_ptr<XrBackend> _backend;
        std::unique_ptr<XrInput> _input;

        bool _active = false;
        XrSessionOptions _options;
        CameraComponent* _camera = nullptr;
        DestroyWatch _cameraWatch;
        XrVisibility _visibility = XrVisibility::Visible;

        // What the session changes on the camera, to give back when it ends.
        float _savedFov = 45.0f;
        float _savedAspectRatio = 1.0f;
        AspectRatioMode _savedAspectRatioMode = AspectRatioMode::ASPECT_AUTO;
        bool _savedHorizontalFov = false;
        Vector3 _savedLocalPosition{0.0f, 0.0f, 0.0f};
        Quaternion _savedLocalRotation{0.0f, 0.0f, 0.0f, 1.0f};

        XrFrameTiming _timing;
        bool _frameWaited = false;
        bool _frameBegun = false;
        bool _havePose = false;
        bool _rendered = false;
        std::vector<XrViewState> _viewStates;
        std::vector<XrView> _views;
        std::vector<XrInputSourceState> _inputStates;
        std::vector<XrViewTarget> _targets;
        std::vector<float> _sliceBoundaries;

        std::shared_ptr<RenderPass> _mirrorPass;
    };
}
