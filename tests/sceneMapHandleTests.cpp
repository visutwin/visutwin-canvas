// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.10.2026
//
// A camera's scene-map handles and what a Compute reads from them.
//
// What is pinned here:
//   - publishing a depth records the texture, the render version, the storage flags (packed and
//     reciprocal only with linear), the camera parameters, the inverse view-projection the
//     shaders were given and the viewport in texels with row 0 at the top, placed from the
//     camera's bottom-left rect as the renderer places it;
//   - a handle is the same object for the camera's life, and marked destroyed with it;
//   - Compute::prepareDispatch binds the published textures and fills its block from the
//     handles; a map never published, or one whose texture its producer has freed, binds a 1x1
//     substitute whose viewport is that one texel, and a detached map binds nothing.
//
// No example shows the substitutes or a camera rect that is not the whole target, so the
// numbers are held here.

#include <algorithm>
#include <iterator>
#include <memory>

#include "platform/graphics/compute.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/graphNode.h"
#include "support/check.h"
#include "support/stubDevice.h"

using namespace visutwin::canvas;
using test::check;

namespace
{
    std::shared_ptr<Texture> makeTexture(GraphicsDevice* device, const uint32_t width, const uint32_t height)
    {
        TextureOptions options;
        options.width = width;
        options.height = height;
        options.format = PixelFormat::PIXELFORMAT_R32F;
        options.mipmaps = false;
        return std::make_shared<Texture>(device, options);
    }

    bool sameMatrix(const Matrix4& a, const Matrix4& b)
    {
        float x[16], y[16];
        a.store(x);
        b.store(y);
        return std::equal(std::begin(x), std::end(x), std::begin(y));
    }

    bool viewportIs(const std::array<uint32_t, 4>& viewport, const uint32_t x, const uint32_t y,
        const uint32_t w, const uint32_t h)
    {
        return viewport[0] == x && viewport[1] == y && viewport[2] == w && viewport[3] == h;
    }

    void depthHandleRecordsThePublication(GraphicsDevice* device)
    {
        Camera camera;
        camera.setNearClip(2.0f);
        camera.setFarClip(500.0f);
        const Matrix4 viewProjection = camera.projectionMatrix() * Matrix4::translation(Vector3(1.0f, 2.0f, 3.0f));
        camera.storeShaderMatrices(viewProjection, 0.0f, 0.0f, 7);

        const auto handle = camera.sceneDepthMapHandle();
        check(handle && handle->texture.expired() && handle->renderVersion == -1,
            "a camera's depth handle starts with no map");

        // the top-right quarter of a 200 x 100 target: its rect origin is the bottom-left corner
        camera.setRect(Vector4(0.5f, 0.5f, 0.5f, 0.5f));
        const auto texture = makeTexture(device, 200, 100);
        camera.publishSceneDepthMap(texture, 7, false, true, true);

        check(handle == camera.sceneDepthMapHandle(), "the handle is the same object after a publication");
        check(handle->texture.lock() == texture && handle->renderVersion == 7, "the texture and render version");
        check(!handle->linear && !handle->packed && !handle->reciprocal,
            "packed and reciprocal are recorded only with linear");
        check(handle->cameraParams[0] == 1.0f / 500.0f && handle->cameraParams[1] == 500.0f &&
            handle->cameraParams[2] == 2.0f && handle->cameraParams[3] == 0.0f,
            "the camera parameters: 1 / far, far, near, perspective");
        check(sameMatrix(handle->viewProjectionInverse, viewProjection.inverse()),
            "the inverse of the view-projection the shaders were given");
        check(viewportIs(handle->viewport, 100, 0, 100, 50), "the top-right quarter starts at row 0");

        camera.setRect(Vector4(0.0f, 0.0f, 0.5f, 0.5f));
        camera.setProjection(ProjectionType::Orthographic);
        camera.publishSceneDepthMap(texture, 8, true, false, true);
        check(viewportIs(handle->viewport, 0, 50, 100, 50), "the bottom-left quarter starts half way down");
        check(handle->linear && !handle->packed && handle->reciprocal, "a linear reciprocal depth");
        check(handle->cameraParams[3] == 1.0f, "an orthographic projection");
    }

    void handlesOutliveTheirCamera(GraphicsDevice* device)
    {
        std::shared_ptr<SceneDepthMapHandle> depth;
        std::shared_ptr<SceneColorMapHandle> color;
        const auto texture = makeTexture(device, 4, 4);
        {
            Camera camera;
            depth = camera.sceneDepthMapHandle();
            color = camera.sceneColorMapHandle();
            camera.publishSceneDepthMap(texture, 1, false, false, false);
            camera.publishSceneColorMap(texture, true);
            check(color->texture.lock() == texture && color->gamma, "the colour handle records the texture and gamma");
            check(!depth->destroyed && !color->destroyed, "a live camera's handles are not destroyed");
        }
        check(depth->destroyed && color->destroyed, "the handles are marked destroyed with their camera");
        check(depth->texture.expired() && color->texture.expired(), "and name no map any more");
    }

    void computeResolvesTheHandles(GraphicsDevice* device)
    {
        Camera camera;
        Compute compute(device, nullptr, "scene-maps-test");
        compute.prepareDispatch();
        check(!compute.hasSceneMaps() && !compute.sceneDepthTexture() && !compute.sceneColorTexture(),
            "a compute with no maps attached binds none");

        compute.setSceneDepthMap(camera.sceneDepthMapHandle());
        compute.setSceneColorMap(camera.sceneColorMapHandle());
        compute.prepareDispatch();
        Texture* depthSubstitute = compute.sceneDepthTexture();
        Texture* colorSubstitute = compute.sceneColorTexture();
        check(depthSubstitute && depthSubstitute->width() == 1 && depthSubstitute->height() == 1,
            "a depth never rendered binds the 1x1 substitute");
        check(colorSubstitute && colorSubstitute->width() == 1 && colorSubstitute != depthSubstitute,
            "and so does a colour, a substitute of its own");
        check(viewportIs({compute.sceneMapsBlock().depthViewport[0], compute.sceneMapsBlock().depthViewport[1],
            compute.sceneMapsBlock().depthViewport[2], compute.sceneMapsBlock().depthViewport[3]}, 0, 0, 1, 1),
            "the substitute's viewport is its one texel");

        auto depth = makeTexture(device, 64, 32);
        auto color = makeTexture(device, 64, 32);
        camera.publishSceneDepthMap(depth, 3, true, false, true);
        camera.publishSceneColorMap(color, true);
        compute.prepareDispatch();
        const ComputeSceneMapsBlock& block = compute.sceneMapsBlock();
        check(compute.sceneDepthTexture() == depth.get() && compute.sceneColorTexture() == color.get(),
            "the published maps are bound");
        check(block.flags[0] == 1u && block.flags[1] == 1u && block.flags[2] == 1u,
            "the block's flags: linear, reciprocal, gamma");
        check(block.depthViewport[2] == 64 && block.depthViewport[3] == 32, "the block's viewport");
        check(block.depthCameraParams[1] == camera.farClip(), "the block's camera parameters");

        // a producer freeing its texture (a rebuilt camera frame) leaves the substitute bound
        // until the next publication
        depth.reset();
        compute.prepareDispatch();
        check(compute.sceneDepthTexture() == depthSubstitute, "a freed depth binds the substitute again");
        check(compute.sceneColorTexture() == color.get(), "while the colour stays bound");

        compute.setSceneColorMap(nullptr);
        compute.prepareDispatch();
        check(!compute.sceneColorTexture(), "a detached colour binds nothing");
    }
}

int main()
{
    test::StubGraphicsDevice device;
    depthHandleRecordsThePublication(&device);
    handlesOutliveTheirCamera(&device);
    computeResolvesTheHandles(&device);
    return test::finish("scene map handles");
}
