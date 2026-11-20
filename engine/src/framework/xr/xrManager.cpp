// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025
//
#include "xrManager.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <spdlog/spdlog.h>

#include "framework/components/camera/cameraComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/renderTarget.h"
#include "scene/camera.h"
#include "scene/graphics/renderPassShaderQuad.h"
#include "xrBackend.h"

namespace visutwin::canvas
{
    namespace
    {
        /// Copies a view's image over the window, so someone beside the headset sees it.
        class XrMirrorPass final : public RenderPassShaderQuad
        {
        public:
            explicit XrMirrorPass(const std::shared_ptr<GraphicsDevice>& device) : RenderPassShaderQuad(device)
            {
                init(nullptr);
            }

            void setSource(Texture* texture)
            {
                _source = texture;
                setEnabled(texture != nullptr);
            }

            void prepareShaders() override { useSlangShader("cookie-blit", "2d"); }

            void execute() override
            {
                if (!_source) {
                    return;
                }
                prepareShaders();
                setQuadTextureBinding(0, _source);
                RenderPassShaderQuad::execute();
            }

        private:
            Texture* _source = nullptr;
        };
    }

    XrManager::XrManager(std::shared_ptr<XrBackend> backend)
        : _backend(std::move(backend)), _input(std::make_unique<XrInput>(this))
    {
    }

    XrManager::~XrManager()
    {
        end();
    }

    void XrManager::setBackend(std::shared_ptr<XrBackend> backend)
    {
        end();
        _backend = std::move(backend);
    }

    bool XrManager::isAvailable(const XrSessionType type) const
    {
        return _backend && _backend->isAvailable(type);
    }

    const XrView* XrManager::view(const XrEye eye) const
    {
        for (const auto& candidate : _views) {
            if (candidate.eye == eye) {
                return &candidate;
            }
        }
        return nullptr;
    }

    void XrManager::fail(const std::string& message, const XrErrorCallback& callback)
    {
        spdlog::error("XR: {}", message);
        fire("error", message);
        if (callback) {
            callback(&message);
        }
    }

    bool XrManager::start(CameraComponent* camera, const XrSessionOptions& options, const XrErrorCallback& callback)
    {
        if (_active) {
            fail("a session is already running", callback);
            return false;
        }
        if (!_backend) {
            fail("no XR backend was supplied", callback);
            return false;
        }
        if (!camera || !camera->camera() || !camera->entity()) {
            fail("a session needs a camera component on an entity", callback);
            return false;
        }
        if (!_engine || !_engine->graphicsDevice()) {
            fail("the XR manager belongs to no engine (AppOptions::xr)", callback);
            return false;
        }
        if (!_backend->isAvailable(options.type)) {
            fail(std::string("the ") + _backend->name() + " backend has no session of that type to offer", callback);
            return false;
        }

        _options = options;
        _options.depthSlices = std::max(options.depthSlices, 1);
        _options.samples = std::max(options.samples, 1);
        if (!_options.depthSliceBoundaries.empty() &&
            (_options.depthSliceBoundaries.size() != static_cast<size_t>(_options.depthSlices) + 1 ||
             !std::ranges::is_sorted(_options.depthSliceBoundaries))) {
            fail("depthSliceBoundaries needs depthSlices + 1 ascending distances", callback);
            return false;
        }

        std::string error;
        if (!_backend->begin(_options, *_engine->graphicsDevice(), error)) {
            fail(error.empty() ? "the backend could not start a session" : error, callback);
            return false;
        }

        _camera = camera;
        Camera* cam = camera->camera();
        Entity* entity = camera->entity();
        _savedFov = cam->fov();
        _savedAspectRatio = cam->aspectRatio();
        _savedAspectRatioMode = cam->aspectRatioMode();
        _savedHorizontalFov = cam->horizontalFov();
        _savedLocalPosition = entity->localPosition();
        _savedLocalRotation = entity->localRotation();
        cam->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
        cam->setHorizontalFov(false);
        // Nothing to draw until the first frame's views arrive.
        cam->setViews({});

        // A camera destroyed under a session ends it: the views belong to that camera.
        _cameraWatch.watch(entity, [this] {
            _camera = nullptr;
            end();
        });

        if (_options.mirror) {
            _mirrorPass = std::make_shared<XrMirrorPass>(_engine->graphicsDevice());
            _mirrorPass->setEnabled(false);
            cam->afterPasses().push_back(_mirrorPass);
        }

        _active = true;
        _frameWaited = false;
        _frameBegun = false;
        _havePose = false;
        _visibility = _backend->visibility();
        spdlog::info("XR session started on the {} backend: {} x {} per view, {} depth slice(s)",
            _backend->name(), _backend->viewSize().first, _backend->viewSize().second, _options.depthSlices);
        fire("start");
        if (callback) {
            callback(nullptr);
        }
        return true;
    }

    void XrManager::restoreCamera()
    {
        _cameraWatch.reset();
        if (!_camera) {
            return;
        }
        if (Camera* cam = _camera->camera()) {
            cam->clearViews();
            cam->setFov(_savedFov);
            cam->setHorizontalFov(_savedHorizontalFov);
            cam->setAspectRatioMode(_savedAspectRatioMode);
            cam->setAspectRatio(_savedAspectRatio);
            if (_mirrorPass) {
                std::erase(cam->afterPasses(), _mirrorPass);
            }
        }
        if (Entity* entity = _camera->entity()) {
            entity->setLocalPosition(_savedLocalPosition);
            entity->setLocalRotation(_savedLocalRotation);
        }
        _camera = nullptr;
    }

    void XrManager::end(const XrErrorCallback& callback)
    {
        if (!_active) {
            if (callback) {
                callback(nullptr);
            }
            return;
        }
        _active = false;
        if (_frameBegun && _backend) {
            XrFrameSubmission submission;
            _backend->endFrame(submission);
            _frameBegun = false;
        }
        restoreCamera();
        _mirrorPass.reset();
        _targets.clear();
        _input->clear();
        _views.clear();
        _viewStates.clear();
        if (_backend) {
            _backend->end();
        }
        fire("end");
        if (callback) {
            callback(nullptr);
        }
    }

    void XrManager::waitFrame()
    {
        if (!_active || _frameWaited || _frameBegun) {
            return;
        }
        if (!_backend->waitFrame(_timing)) {
            _timing.shouldRender = false;
        }
        _frameWaited = true;
    }

    bool XrManager::update(const float dt)
    {
        if (!_active) {
            return false;
        }
        if (!_backend->running()) {
            end();
            return false;
        }
        waitFrame();

        if (const XrVisibility visibility = _backend->visibility(); visibility != _visibility) {
            _visibility = visibility;
            fire("visibility:change", visibility);
        }

        Camera* cam = _camera->camera();
        Entity* entity = _camera->entity();

        _viewStates.clear();
        _havePose = _backend->locateViews(_viewStates) && !_viewStates.empty();
        if (_havePose) {
            const float nearClip = cam->nearClip();
            const float farClip = cam->farClip();
            _views.resize(_viewStates.size());
            Vector3 centre(0.0f, 0.0f, 0.0f);
            for (size_t i = 0; i < _viewStates.size(); ++i) {
                const XrViewState& state = _viewStates[i];
                XrView& view = _views[i];
                view.eye = state.eye;
                view.pose = state.pose;
                view.fov = state.fov;
                view.projection = xrProjectionFromFov(state.fov, nearClip, farClip);
                view.viewInvMat = Matrix4::trs(state.pose.position, state.pose.rotation, Vector3(1.0f, 1.0f, 1.0f));
                view.viewMat = view.viewInvMat.inverse();
                centre = centre + state.pose.position;
            }
            centre = centre * (1.0f / static_cast<float>(_viewStates.size()));

            // The camera sits between its views, turned as the first: what culling's
            // neighbours (sort distances, shadow fitting, scripts) take as the viewer.
            entity->setLocalPosition(centre);
            entity->setLocalRotation(_viewStates[0].pose.rotation);

            // Its own projection follows the first view's, for everything that reads the
            // camera rather than a view.
            const XrFov& fov = _viewStates[0].fov;
            const float height = std::tan(fov.up) - std::tan(fov.down);
            const float width = std::tan(fov.right) - std::tan(fov.left);
            if (height > 0.0f && width > 0.0f) {
                cam->setFov(2.0f * std::atan(height * 0.5f) * 180.0f / std::numbers::pi_v<float>);
                cam->setAspectRatio(width / height);
            }
        }

        _inputStates.clear();
        _backend->pollInput(_inputStates);
        _input->update(_inputStates, dt);

        fire("update");
        return _havePose;
    }

    void XrManager::beginRender()
    {
        if (!_active || _frameBegun) {
            return;
        }
        // An application that never updated this frame still owes the runtime the wait.
        waitFrame();
        _frameWaited = false;
        _frameBegun = true;
        _rendered = false;

        Camera* cam = _camera->camera();
        Entity* entity = _camera->entity();
        const int slices = _options.depthSlices;

        _targets.clear();
        const bool haveTargets = _backend->beginFrame(_targets);

        std::vector<CameraView> cameraViews;
        const size_t expected = _views.size() * static_cast<size_t>(slices);
        if (haveTargets && _havePose && _timing.shouldRender && !_views.empty() && _targets.size() == expected) {
            GraphNode* parent = entity->parent();
            const Matrix4 parentWorld = parent ? parent->worldTransform() : Matrix4::identity();
            _sliceBoundaries = !_options.depthSliceBoundaries.empty()
                ? _options.depthSliceBoundaries
                : xrDepthSliceBoundaries(cam->nearClip(), cam->farClip(), slices);

            cameraViews.reserve(expected);
            for (size_t v = 0; v < _views.size(); ++v) {
                XrView& view = _views[v];
                view.viewInvOffMat = parentWorld * view.viewInvMat;
                view.viewOffMat = view.viewInvOffMat.inverse();
                view.viewport = _targets[v * static_cast<size_t>(slices)].viewport;
                for (int s = 0; s < slices; ++s) {
                    const XrViewTarget& target = _targets[v * static_cast<size_t>(slices) + static_cast<size_t>(s)];
                    CameraView cameraView;
                    cameraView.projection = slices == 1 ? view.projection
                        : xrProjectionFromFov(view.fov, _sliceBoundaries[static_cast<size_t>(s)],
                            _sliceBoundaries[static_cast<size_t>(s) + 1]);
                    cameraView.view = view.viewOffMat;
                    cameraView.viewInverse = view.viewInvOffMat;
                    cameraView.viewport = target.viewport;
                    cameraView.renderTarget = target.target;
                    // Only the farthest slice holds what lies at infinity; the others are
                    // layers over it.
                    cameraView.background = s == slices - 1;
                    cameraViews.push_back(std::move(cameraView));
                }
            }
            _rendered = true;
        } else if (haveTargets && _havePose && _timing.shouldRender && _targets.size() != expected) {
            spdlog::warn("XR: the backend gave {} targets for {} views of {} slice(s); the frame is not drawn",
                _targets.size(), _views.size(), slices);
        }
        cam->setViews(std::move(cameraViews));

        if (_mirrorPass) {
            // The farthest slice of the first view: the whole view when there is one slice.
            static_cast<XrMirrorPass*>(_mirrorPass.get())->setSource(
                _rendered ? _backend->viewTexture(0, slices - 1) : nullptr);
        }
    }

    void XrManager::finishRender()
    {
        if (_active && _frameBegun) {
            _backend->finishFrame();
        }
    }

    void XrManager::endRender()
    {
        if (!_active || !_frameBegun) {
            return;
        }
        _frameBegun = false;
        XrFrameSubmission submission;
        submission.views = _viewStates;
        submission.nearClip = _camera->camera()->nearClip();
        submission.farClip = _camera->camera()->farClip();
        submission.depthSlices = _options.depthSlices;
        submission.depthSliceBoundaries = _sliceBoundaries;
        submission.rendered = _rendered;
        _backend->endFrame(submission);
        _targets.clear();
    }
}
