// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
// SceneDepthReader: each queued read is a dispatch of `scene-depth-read` into a storage texture of
// its own (R32F, one texel a sample) in a compute pass among the camera's after passes, so it runs
// right after the camera renders, while its depth is the one the camera published; the texture is
// then read back asynchronously and handed to the callback. A read's texture and compute live
// until its callback has run.
//
#include "sceneDepthReader.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "platform/graphics/compute.h"
#include "platform/graphics/computePass.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/sceneMapHandles.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/shader-lib/slangShaders.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        struct DepthReadParams
        {
            float rect[4];
            uint32_t grid[4];
        };
    }

    struct SceneDepthReader::Request
    {
        Vector4 rect;
        uint32_t width = 0;
        uint32_t height = 0;
        Callback callback;
        std::shared_ptr<Texture> samples;
        std::unique_ptr<Compute> compute;
    };

    struct SceneDepthReader::State
    {
        // Queued by read(), dispatched by the next pass, delivered by the read back.
        std::vector<std::shared_ptr<Request>> queued;
        std::vector<std::shared_ptr<Request>> dispatched;
        size_t inFlight = 0;
        // Whether the camera rendered a depth in the frame it last rendered.
        bool depthRendered = true;
    };

    SceneDepthReader::SceneDepthReader(const std::shared_ptr<GraphicsDevice>& device, Camera* camera)
        : _device(device), _camera(camera), _state(std::make_shared<State>())
    {
        if (!_device || !_camera) {
            return;
        }
        if (_device->supportsCompute()) {
            _shader = getOrCreateSlangShader(_device.get(), "scene-depth-read");
        }
        _pass = std::make_shared<ComputePass>(_device);
        _pass->setName("SceneDepthRead");
        _pass->setOnBefore([this](RenderPass& pass) {
            dispatchRequests();
            pass.setExecuteEnabled(!_pass->computes().empty());
        });
        // The dispatches are encoded by now: queue each texture's read back.
        _pass->setOnAfter([state = _state](RenderPass&) {
            for (auto& request : state->dispatched) {
                ++state->inFlight;
                const bool queued = request->samples->readAsync(
                    [state, request](const uint8_t* data, const size_t size) {
                        --state->inFlight;
                        std::vector<float> samples(static_cast<size_t>(request->width) * request->height,
                            std::numeric_limits<float>::infinity());
                        std::memcpy(samples.data(), data, std::min(size, samples.size() * sizeof(float)));
                        request->callback(samples);
                    });
                if (!queued) {
                    --state->inFlight;
                    request->callback(std::vector<float>(static_cast<size_t>(request->width) * request->height,
                        std::numeric_limits<float>::infinity()));
                }
            }
            state->dispatched.clear();
        });
        _camera->afterPasses().push_back(_pass);
    }

    SceneDepthReader::~SceneDepthReader()
    {
        if (_camera && _pass) {
            std::erase(_camera->afterPasses(), _pass);
        }
    }

    bool SceneDepthReader::read(const Vector4& rect, const uint32_t width, const uint32_t height, Callback callback)
    {
        if (!_shader || !_pass || width == 0 || height == 0 || !callback || !_state->depthRendered) {
            return false;
        }
        auto request = std::make_shared<Request>();
        request->rect = rect;
        request->width = width;
        request->height = height;
        request->callback = std::move(callback);
        _state->queued.push_back(std::move(request));
        return true;
    }

    size_t SceneDepthReader::pendingReads() const
    {
        return _state->queued.size() + _state->dispatched.size() + _state->inFlight;
    }

    void SceneDepthReader::dispatchRequests()
    {
        _pass->computes().clear();
        const auto& handle = _camera->sceneDepthMapHandle();
        // The camera has just rendered: its record is this frame's if it rendered a depth at all.
        _state->depthRendered = handle && !handle->texture.expired() &&
            handle->renderVersion == _device->renderVersion();
        if (_state->queued.empty()) {
            return;
        }
        if (!_state->depthRendered) {
            // nothing rendered the depth, so nothing was hit anywhere
            for (const auto& request : _state->queued) {
                request->callback(std::vector<float>(static_cast<size_t>(request->width) * request->height,
                    std::numeric_limits<float>::infinity()));
            }
            _state->queued.clear();
            return;
        }

        for (auto& request : _state->queued) {
            TextureOptions options;
            options.name = "SceneDepthRead";
            options.width = request->width;
            options.height = request->height;
            options.format = PixelFormat::PIXELFORMAT_R32F;
            options.mipmaps = false;
            options.storage = true;
            options.minFilter = FilterMode::FILTER_NEAREST;
            options.magFilter = FilterMode::FILTER_NEAREST;
            request->samples = std::make_shared<Texture>(_device.get(), options);

            request->compute = std::make_unique<Compute>(_device.get(), _shader, "SceneDepthRead");
            request->compute->setSceneDepthMap(handle);
            request->compute->setParameter("samples", request->samples.get());
            const DepthReadParams params{{request->rect.getX(), request->rect.getY(), request->rect.getZ(),
                request->rect.getW()}, {request->width, request->height, 0u, 0u}};
            request->compute->setUniformBlock(&params, sizeof(params));
            request->compute->setupDispatchThreads(request->width, request->height);
            _pass->computes().push_back(request->compute.get());
            _state->dispatched.push_back(request);
        }
        _state->queued.clear();
    }
}
