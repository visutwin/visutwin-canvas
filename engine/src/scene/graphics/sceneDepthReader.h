// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 31.10.2026
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "core/math/vector4.h"

namespace visutwin::canvas
{
    class Camera;
    class Compute;
    class ComputePass;
    class GraphicsDevice;
    class Shader;
    class Texture;

    /**
     * Reads a camera's scene depth back to the CPU over a grid of samples, without stalling. A read
     * names a region of the camera's viewport (normalized, from its bottom-left corner) and a
     * number of samples across and up; each sample is the linear camera depth in world units at
     * the centre of its cell, decoded from whatever its producer stored (the depth grab, a camera
     * frame's scene pass or its prepass: SceneDepthMapHandle), or infinity where nothing was
     * rendered. The reads are serviced after the camera renders (a compute pass among its after
     * passes, the Slang program `scene-depth-read`) and delivered at a later frame start
     * (Texture::readAsync), rows from the bottom of the region up.
     *
     * The camera must render a scene depth (requestSceneDepthMap, or a camera frame with a depth
     * consumer); a frame in which it rendered none delivers every sample as infinity, and after
     * it read() refuses until a depth is rendered again.
     */
    class SceneDepthReader
    {
    public:
        using Callback = std::function<void(const std::vector<float>& samples)>;

        SceneDepthReader(const std::shared_ptr<GraphicsDevice>& device, Camera* camera);
        ~SceneDepthReader();

        SceneDepthReader(const SceneDepthReader&) = delete;
        SceneDepthReader& operator=(const SceneDepthReader&) = delete;

        /// Queues a read of `width` x `height` samples over `rect`; false, queuing nothing, when
        /// the camera rendered no depth in the frame it last rendered, or the device has no compute.
        bool read(const Vector4& rect, uint32_t width, uint32_t height, Callback callback);

        /// Reads still to be dispatched or delivered.
        size_t pendingReads() const;

    private:
        struct Request;
        struct State;

        void dispatchRequests();

        std::shared_ptr<GraphicsDevice> _device;
        Camera* _camera = nullptr;
        std::shared_ptr<Shader> _shader;
        std::shared_ptr<ComputePass> _pass;
        // Shared with the read callbacks, which may outlive the reader.
        std::shared_ptr<State> _state;
    };
}
