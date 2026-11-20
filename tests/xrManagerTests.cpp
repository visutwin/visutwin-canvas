// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
// The XR layer without a GPU: the projection built from a field of view, the depth-slice
// boundaries, the frustum a multi-view camera is culled with, the session's life on a stub
// device (start, frames, a camera destroyed under it, a session that cannot start), and the
// OpenXR backend on a machine with no runtime, which must say so and start nothing.
//
// The OpenXR backend reaches the Metal device for its graphics binding, so this executable
// supplies the metal-cpp *_PRIVATE_IMPLEMENTATION translation unit, as every program linking
// the static engine and using Metal does (see examples/exampleApp.cpp).
#ifdef VISUTWIN_HAS_METAL
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <QuartzCore/QuartzCore.hpp>
#endif

#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/math/primitives.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/camera/cameraComponentSystem.h"
#include "framework/xr/openxr/openXrBackend.h"
#include "framework/xr/simulatedXrBackend.h"
#include "framework/xr/xrManager.h"
#include "scene/camera.h"
#include "scene/frustumUtils.h"
#include "scene/graphNode.h"
#include "support/check.h"
#include "support/stubDevice.h"
#include "support/testEngine.h"

using namespace visutwin::canvas;
using namespace visutwin::canvas::test;

namespace
{
    void checkProjection()
    {
        // A symmetric field of view is the camera's own perspective projection.
        const XrFov symmetric{-0.5f, 0.5f, 0.5f, -0.5f};
        const Matrix4 fromFov = xrProjectionFromFov(symmetric, 0.1f, 100.0f);
        const float fovDegrees = 2.0f * 0.5f * 180.0f / 3.14159265358979f;
        const Matrix4 perspective = Matrix4::perspective(fovDegrees, 1.0f, 0.1f, 100.0f);
        bool same = true;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                same = same && near(fromFov.getElement(c, r), perspective.getElement(c, r), 1e-4f);
            }
        }
        check(same, "a symmetric field of view gives the perspective projection");

        // An asymmetric one puts the direction of its left edge on the clip volume's left side.
        const XrFov skewed{-0.9f, 0.6f, 0.7f, -0.8f};
        const Matrix4 projection = xrProjectionFromFov(skewed, 0.1f, 100.0f);
        const Vector4 leftEdge = projection * Vector4(std::tan(skewed.left) * 5.0f, 0.0f, -5.0f, 1.0f);
        const Vector4 topEdge = projection * Vector4(0.0f, std::tan(skewed.up) * 5.0f, -5.0f, 1.0f);
        check(near(leftEdge.getX() / leftEdge.getW(), -1.0f, 1e-4f) && near(topEdge.getY() / topEdge.getW(), 1.0f, 1e-4f),
            "an asymmetric field of view maps its edges to the clip volume's sides");
    }

    void checkSlices()
    {
        const auto one = xrDepthSliceBoundaries(0.1f, 100.0f, 1);
        check(one.size() == 2 && one[0] == 0.1f && one[1] == 100.0f, "one slice is the whole range");
        const auto four = xrDepthSliceBoundaries(0.25f, 100.0f, 4);
        bool ascending = four.size() == 5 && four.front() == 0.25f && four.back() == 100.0f;
        bool evenDioptres = ascending;
        for (size_t i = 1; ascending && i < four.size(); ++i) {
            ascending = four[i] > four[i - 1];
            evenDioptres = evenDioptres && near(1.0f / four[i - 1] - 1.0f / four[i], (4.0f - 0.01f) / 4.0f, 1e-3f);
        }
        check(ascending && evenDioptres, "slices are even steps in dioptres, nearest first");
    }

    void checkCullFrustum()
    {
        // Two eyes 1 m apart looking down -Z, 45 degrees each way. A point 0.9 m left of the
        // left eye at 1 m depth is inside the left eye's view only: the camera's cull frustum
        // must hold it, and must still reject a point far outside both.
        Camera camera;
        GraphNode node;
        camera.setNode(&node);
        const XrFov fov{-0.7854f, 0.7854f, 0.7854f, -0.7854f};
        std::vector<CameraView> views(2);
        for (int eye = 0; eye < 2; ++eye) {
            const float x = eye == 0 ? -0.5f : 0.5f;
            views[eye].projection = xrProjectionFromFov(fov, 0.1f, 100.0f);
            views[eye].viewInverse = Matrix4::translation(x, 0.0f, 0.0f);
            views[eye].view = views[eye].viewInverse.inverse();
        }
        camera.setViews(views);
        const Frustum frustum = buildCameraFrustum(&camera, &node);
        check(frustum.checkPoint(Vector3(-1.4f, 0.0f, -1.0f)), "the cull frustum holds what only the left eye sees");
        check(frustum.checkPoint(Vector3(1.4f, 0.0f, -1.0f)), "and what only the right eye sees");
        check(!frustum.checkPoint(Vector3(-3.0f, 0.0f, -1.0f)) && !frustum.checkPoint(Vector3(0.0f, 0.0f, 1.0f)),
            "and rejects what neither does");

        Frustum single;
        single.create(views[1].projection * views[1].view);
        check(!single.checkPoint(Vector3(-1.4f, 0.0f, -1.0f)), "which one eye's frustum alone would have culled");
    }

    void checkSession()
    {
        StubDeviceOptions deviceOptions;
        deviceOptions.size = {64, 64};
        deviceOptions.renderTargets = true;
        const auto device = std::make_shared<StubGraphicsDevice>(deviceOptions);
        const auto backend = std::make_shared<SimulatedXrBackend>();
        const auto xr = std::make_shared<XrManager>(backend);
        auto engine = makeTestEngine<CameraComponentSystem>(device, [&](AppOptions& options) { options.xr = xr; });

        int errors = 0;
        xr->on("error", [&](const std::string&) { ++errors; });
        check(!xr->start(nullptr) && errors == 1 && !xr->active(), "a session without a camera does not start");

        auto* entity = addTo(engine->root(), newEntity(engine.get(), "camera"));
        auto* component = static_cast<CameraComponent*>(entity->addComponent<CameraComponent>());

        XrSessionOptions bad;
        bad.depthSlices = 3;
        bad.depthSliceBoundaries = {0.1f, 1.0f, 100.0f};
        check(!xr->start(component, bad) && errors == 2, "nor one whose slice boundaries do not match its slices");

        check(xr->start(component) && xr->active() && xr->camera() == component, "a session starts on a stub device");
        check(!xr->start(component) && errors == 3, "a second session is refused while one runs");

        backend->setTracking(false);
        engine->update(0.016f);
        engine->render();
        check(component->camera()->views().empty() && !backend->lastSubmission().rendered &&
            backend->framesBegun() == 1 && backend->framesEnded() == 1,
            "a frame without a pose draws nothing and is still begun and ended");

        backend->setTracking(true);
        backend->setHeadPose({Vector3(1.0f, 1.5f, -2.0f), Quaternion(0.0f, 0.0f, 0.0f, 1.0f)});
        engine->update(0.016f);
        engine->render();
        check(component->camera()->views().size() == 2 && backend->lastSubmission().rendered, "a frame with one draws both views");
        check(near(entity->localPosition(), Vector3(1.0f, 1.5f, -2.0f), 1e-5f), "and the camera sits at the head");
        const XrView* left = xr->view(XrEye::Left);
        check(left && near(left->viewInvOffMat.getTranslation(), Vector3(1.0f - 0.032f, 1.5f, -2.0f), 1e-5f),
            "the left eye half the eye distance to its left");

        // The camera's entity destroyed under the session ends it.
        int ended = 0;
        xr->on("end", [&] { ++ended; });
        entity->destroy();
        check(!xr->active() && ended == 1 && xr->camera() == nullptr, "destroying the camera ends the session");
        engine->update(0.016f);
        engine->render();

        engine->destroy();
    }

    /// The OpenXR loader is built without RTTI and carries its own copies of the standard
    /// exceptions' type information. Linked as a plain static library on macOS, those copies
    /// took over this executable's references, and a standard exception thrown by the C++
    /// runtime matched no `catch (const std::exception&)` here: the process aborted. The
    /// engine therefore links the loader with its symbols hidden; were that undone, this
    /// check does not fail, it terminates.
    void checkStandardExceptionsStillCatch()
    {
        bool caught = false;
        try {
            const std::vector<int> empty;
            (void)empty.at(1);
        } catch (const std::exception&) {
            caught = true;
        }
        check(caught, "a standard exception is caught as std::exception with the OpenXR loader linked in");
    }

    void checkOpenXrWithoutRuntime()
    {
#ifdef VISUTWIN_XR_OPENXR
        // A runtime manifest that does not exist: the loader finds no runtime, whatever the machine has.
        setenv("XR_RUNTIME_JSON", "/nonexistent/visutwin-no-runtime.json", 1);
        const auto backend = createOpenXrBackend();
        check(backend != nullptr, "the OpenXR backend is built in");
        if (!backend) {
            return;
        }
        check(!backend->isAvailable(XrSessionType::Vr) && !backend->isAvailable(XrSessionType::Ar),
            "with no runtime it has no session to offer");

        const auto device = std::make_shared<StubGraphicsDevice>();
        const auto xr = std::make_shared<XrManager>(backend);
        auto engine = makeTestEngine<CameraComponentSystem>(device, [&](AppOptions& options) { options.xr = xr; });
        auto* entity = addTo(engine->root(), newEntity(engine.get(), "camera"));
        auto* component = static_cast<CameraComponent*>(entity->addComponent<CameraComponent>());
        std::string reason;
        check(!xr->start(component, {}, [&](const std::string* error) { reason = error ? *error : ""; }) &&
            !xr->active() && !reason.empty() && !component->camera()->multiView(),
            "and a session does not start, with the reason given: " + reason);
        engine->update(0.016f);
        engine->render();
        engine->destroy();
#else
        check(createOpenXrBackend() == nullptr, "without OpenXR in the build there is no OpenXR backend");
#endif
    }
}

int main()
{
    checkProjection();
    checkSlices();
    checkCullFrustum();
    checkSession();
    checkStandardExceptionsStillCatch();
    checkOpenXrWithoutRuntime();
    return finish("xr manager");
}
