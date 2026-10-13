// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
#include "immediateBatch.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "lineWriter.h"
#include "core/shape/boundingBox.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/layer.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr size_t kFloats = LineWriter::kFloatsPerVertex;
        constexpr size_t kVertexBytes = kFloats * sizeof(float);
    }

    ImmediateBatch::ImmediateBatch(GraphicsDevice* device, std::shared_ptr<Material> material, GraphNode* node)
        : _device(device), _material(std::move(material))
    {
        // Position at attribute 0, colour at 5: the layout both backends read for 28 bytes.
        _format = std::make_shared<VertexFormat>(static_cast<int>(kVertexBytes), VertexFormat::pointElements());
        _mesh = std::make_shared<Mesh>();
        // Lines reach wherever their points do, so the bounds are never culled against; the
        // transparent sort reads the centre.
        _mesh->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f)));
        _meshInstance = std::make_unique<MeshInstance>(_mesh, _material, node);
        _meshInstance->setCull(false);
        _meshInstance->setCastShadow(false);
        _meshInstance->setReceiveShadow(false);
        _meshInstance->setVisible(false);
    }

    ImmediateBatch::~ImmediateBatch()
    {
        setLayer(nullptr);
    }

    void ImmediateBatch::setLayer(const std::shared_ptr<Layer>& layer)
    {
        if (layer == _layer) {
            return;
        }
        if (_layer) {
            _layer->removeMeshInstances({_meshInstance.get()});
        }
        _layer = layer;
        if (_layer) {
            _layer->addMeshInstances({_meshInstance.get()});
        }
    }

    void ImmediateBatch::reserve(const size_t count)
    {
        const size_t required = _vertexCount + count;
        if (required <= _capacity) {
            return;
        }
        size_t capacity = std::max(_capacity, kMinVertexCapacity);
        while (capacity < required) {
            capacity *= 2;
        }
        setCapacity(capacity);
    }

    void ImmediateBatch::setCapacity(const size_t capacity)
    {
        // resize keeps the vertices written so far; a shrink happens only when the batch is
        // empty, at the end of a frame.
        _vertices.resize(capacity * kFloats);
        _vertices.shrink_to_fit();
        _capacity = capacity;
    }

    size_t ImmediateBatch::allocate(const size_t count)
    {
        reserve(count);
        const size_t first = _vertexCount;
        _vertexCount += count;
        return first;
    }

    void ImmediateBatch::addLines(const Vector3* positions, const size_t count, const Color* colors,
        const Color& color)
    {
        const size_t first = allocate(count);
        float* v = _vertices.data() + first * kFloats;
        for (size_t i = 0; i < count; ++i, v += kFloats) {
            const Color& c = colors ? colors[i] : color;
            v[0] = positions[i].getX();
            v[1] = positions[i].getY();
            v[2] = positions[i].getZ();
            v[3] = c.r;
            v[4] = c.g;
            v[5] = c.b;
            v[6] = c.a;
        }
    }

    void ImmediateBatch::addLineArrays(const float* positions, const size_t floatCount, const float* colors,
        const Color& color)
    {
        const size_t count = floatCount / 3;
        const size_t first = allocate(count);
        float* v = _vertices.data() + first * kFloats;
        for (size_t i = 0; i < count; ++i, v += kFloats) {
            v[0] = positions[i * 3];
            v[1] = positions[i * 3 + 1];
            v[2] = positions[i * 3 + 2];
            if (colors) {
                std::memcpy(v + 3, colors + i * 4, 4 * sizeof(float));
            } else {
                v[3] = color.r;
                v[4] = color.g;
                v[5] = color.b;
                v[6] = color.a;
            }
        }
    }

    void ImmediateBatch::upload(const size_t slot)
    {
        if (_vertexCount == 0 || !_device) {
            _meshInstance->setVisible(false);
            return;
        }
        if (_ring.size() < static_cast<size_t>(std::max(_device->maxFramesInFlight(), 1))) {
            _ring.resize(static_cast<size_t>(std::max(_device->maxFramesInFlight(), 1)));
        }
        RingSlot& ring = _ring[slot % _ring.size()];
        const size_t bytes = _vertexCount * kVertexBytes;
        // A slot is recreated when it is too small, or larger than the storage now is (the
        // storage shrank), at the storage's capacity; otherwise this frame's vertices go into
        // its front, and the draw's count says how much of it is live.
        if (!ring.buffer || ring.capacity < _vertexCount || ring.capacity > _capacity) {
            VertexBufferOptions options;
            options.usage = BUFFER_DYNAMIC;
            options.data.assign(_capacity * kVertexBytes, 0);
            std::memcpy(options.data.data(), _vertices.data(), bytes);
            ring.buffer = _device->createVertexBuffer(_format, static_cast<int>(_capacity), options);
            ring.capacity = ring.buffer ? _capacity : 0;
        } else if (!ring.buffer->writeRange(0, _vertices.data(), bytes)) {
            ring.buffer.reset();
            ring.capacity = 0;
        }
        if (!ring.buffer) {
            _meshInstance->setVisible(false);
            return;
        }

        _mesh->setVertexBuffer(ring.buffer);
        Primitive primitive;
        primitive.type = PRIMITIVE_LINES;
        primitive.base = 0;
        primitive.count = static_cast<int>(_vertexCount);
        primitive.indexed = false;
        _mesh->setPrimitive(primitive, 0);
        _meshInstance->setVisible(true);
    }

    void ImmediateBatch::clear()
    {
        if (_vertexCount * 2 > _capacity) {
            // This frame justified the storage: restart the wait and the peak.
            _framesSinceHighUse = 0;
            _peakVertexCount = 0;
        } else {
            ++_framesSinceHighUse;
            _peakVertexCount = std::max(_peakVertexCount, _vertexCount);
        }
        _vertexCount = 0;
        _meshInstance->setVisible(false);

        // Shrink to fit the largest of the quiet frames, not the last one, so a batch whose use
        // varies does not have to grow again at once.
        if (_framesSinceHighUse >= kShrinkFrameDelay && _capacity > kMinVertexCapacity) {
            size_t capacity = kMinVertexCapacity;
            while (capacity < _peakVertexCount) {
                capacity *= 2;
            }
            if (capacity < _capacity) {
                setCapacity(capacity);
            }
            _peakVertexCount = 0;
            _framesSinceHighUse = 0;
        }
    }
}
