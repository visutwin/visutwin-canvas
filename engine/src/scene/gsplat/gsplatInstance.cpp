// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "gsplatInstance.h"

#include <cstring>

#include "gsplatResource.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"

namespace visutwin::canvas
{
    GSplatInstance::GSplatInstance(const std::shared_ptr<GSplatResource>& resource)
        : _resource(resource)
    {
        _sorter = std::make_unique<GSplatSorter>(resource->data().centers());

        // Identity initial order so the first frames (before the first sort lands)
        // still draw everything.
        const int numSplats = resource->numSplats();
        std::vector<uint32_t> identity(static_cast<size_t>(numSplats));
        for (int i = 0; i < numSplats; ++i) {
            identity[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
        }
        std::vector<uint8_t> orderBytes(identity.size() * sizeof(uint32_t));
        std::memcpy(orderBytes.data(), identity.data(), orderBytes.size());

        auto orderFormat = std::make_shared<VertexFormat>(static_cast<int>(sizeof(uint32_t)), true, false);
        for (auto& buffer : _orderBuffers) {
            VertexBufferOptions options;
            options.data = orderBytes;
            buffer = resource->device()->createVertexBuffer(orderFormat, numSplats, options);
        }
        _visibleCount = static_cast<uint32_t>(numSplats);
    }

    void GSplatInstance::update(const Vector3& cameraPosition, const Vector3& cameraForward,
        const Matrix4& model, const Matrix4& view, const Matrix4& projection,
        const float viewportWidth, const float viewportHeight)
    {
        // Sort in splat model space: transform the camera into local coordinates.
        const Matrix4 invModel = model.inverse();
        const Vector3 localPosition = invModel.transformPoint(cameraPosition);
        // Direction via point difference (affine transform, no transformVector API).
        const Vector3 localDirection =
            (invModel.transformPoint(cameraPosition + cameraForward) - localPosition).normalized();
        _sorter->setCamera(localPosition, localDirection);

        // Adopt a finished sort: upload into the inactive buffer and swap.
        uint32_t visibleCount = 0;
        if (_sorter->fetchResult(_fetchScratch, visibleCount)) {
            const int inactive = 1 - _activeOrderBuffer;
            std::vector<uint8_t> bytes(_fetchScratch.size() * sizeof(uint32_t));
            std::memcpy(bytes.data(), _fetchScratch.data(), bytes.size());
            if (_orderBuffers[inactive]->setData(bytes)) {
                _activeOrderBuffer = inactive;
                _visibleCount = visibleCount;
            }
        }

        _gpuParams.modelView = view * model;
        _gpuParams.projection = projection;
        _gpuParams.viewport[0] = viewportWidth;
        _gpuParams.viewport[1] = viewportHeight;
        _gpuParams.viewport[2] = viewportWidth > 0.0f ? 1.0f / viewportWidth : 0.0f;
        _gpuParams.viewport[3] = viewportHeight > 0.0f ? 1.0f / viewportHeight : 0.0f;
        _gpuParams.splatCount = static_cast<uint32_t>(_resource->numSplats());
    }
}
