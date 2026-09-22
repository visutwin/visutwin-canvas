// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 11.07.2026.
//
#include "gsplatInstance.h"

#include <algorithm>
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
        const auto& device = resource->device();
        const size_t numOrderBuffers =
            static_cast<size_t>(std::max(1, device->maxFramesInFlight()));
        _orderBuffers.resize(numOrderBuffers);
        for (auto& buffer : _orderBuffers) {
            VertexBufferOptions options;
            options.data = orderBytes;
            buffer = device->createVertexBuffer(orderFormat, numSplats, options);
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

        // The sort key is dot(localCenter, localDirection), so localDirection has to
        // weight each local axis the way the model matrix does. Weighting by the
        // model's own basis vectors projected on the view direction gives exactly
        // dot(model * localCenter, cameraForward) — the true world-space depth — for
        // ANY affine transform.
        //
        // Transforming the view direction by the INVERSE instead (upstream's older
        // form, and what this did) weights axis i by 1/s_i where the true depth
        // weights it by s_i; the two cancel only when every scale is equal. Under a
        // non-uniform scale the error depends on the view direction, so such a splat
        // sorts wrongly against the rest of the scene at some camera angles and not
        // others, with camera distance making no difference (upstream #9268).
        const Vector3 localDirection = sortDirection(model, cameraForward);
        _sorter->setCamera(localPosition, localDirection);

        // Adopt a finished sort: upload into the next buffer of the cycle and make
        // it active. The buffer being written is the one furthest back in the
        // cycle, so the frames still in flight — which can only be holding the one
        // or two written before it — keep reading intact data.
        //
        // At most one upload per frame, or those frames' buffers are no longer the
        // ones immediately before this write and the cycle stops being long enough.
        const int renderVersion = _resource->device()->renderVersion();
        uint32_t visibleCount = 0;
        if (renderVersion != _lastUploadVersion &&
            _sorter->fetchResult(_fetchScratch, visibleCount)) {
            const size_t next = (_activeOrderBuffer + 1) % _orderBuffers.size();
            std::vector<uint8_t> bytes(_fetchScratch.size() * sizeof(uint32_t));
            std::memcpy(bytes.data(), _fetchScratch.data(), bytes.size());
            if (_orderBuffers[next]->setData(bytes)) {
                _activeOrderBuffer = next;
                _visibleCount = visibleCount;
                _lastUploadVersion = renderVersion;
            }
        }

        _gpuParams.modelView = view * model;
        _gpuParams.projection = projection;
        _gpuParams.viewport[0] = viewportWidth;
        _gpuParams.viewport[1] = viewportHeight;
        _gpuParams.viewport[2] = viewportWidth > 0.0f ? 1.0f / viewportWidth : 0.0f;
        _gpuParams.viewport[3] = viewportHeight > 0.0f ? 1.0f / viewportHeight : 0.0f;
        _gpuParams.splatCount = static_cast<uint32_t>(_resource->numSplats());
        _gpuParams.shBands = static_cast<uint32_t>(_resource->data().shBands());
    }

    Vector3 GSplatInstance::sortDirection(const Matrix4& model, const Vector3& cameraForward)
    {
        // Column-major: columns 0/1/2 are the transformed X/Y/Z axes, and weight i is
        // the camera forward projected onto axis i.
        const Vector3 weights(
            Vector3(model.getColumn(0)).dot(cameraForward),
            Vector3(model.getColumn(1)).dot(cameraForward),
            Vector3(model.getColumn(2)).dot(cameraForward));

        // Normalising is order-preserving — every key and the min/max bounds share
        // the positive factor — and keeps the sorter's fixed camera-movement epsilon
        // comparing a unit vector, as it did before.
        const float length = weights.length();
        return length > 1e-8f ? weights * (1.0f / length) : Vector3(0.0f, 0.0f, 1.0f);
    }
}
