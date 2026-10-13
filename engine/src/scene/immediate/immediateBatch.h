// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2026
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class GraphNode;
    class Layer;
    class Material;
    class Mesh;
    class MeshInstance;
    class VertexBuffer;
    class VertexFormat;

    /**
     * The lines one layer draws with one depth-test mode in one frame: CPU storage written
     * through Immediate, uploaded once a frame into a ring of `maxFramesInFlight` vertex
     * buffers (on Metal a frame in flight still reads the previous ones), and drawn as one
     * PRIMITIVE_LINES mesh instance the batch keeps registered with its layer. The storage
     * outlives the frame so the next one need not grow it again, and shrinks back after a long
     * run of frames that used under half of it.
     */
    class ImmediateBatch
    {
    public:
        /// The smallest storage a batch keeps once used, in vertices.
        static constexpr size_t kMinVertexCapacity = 256;
        /// Frames a batch must go without needing half its storage before it shrinks.
        static constexpr int kShrinkFrameDelay = 100;

        ImmediateBatch(GraphicsDevice* device, std::shared_ptr<Material> material, GraphNode* node);
        ~ImmediateBatch();

        ImmediateBatch(const ImmediateBatch&) = delete;
        ImmediateBatch& operator=(const ImmediateBatch&) = delete;

        /// Room for `count` more vertices; returns the first one's index. Invalidates pointers
        /// into the storage taken before.
        size_t allocate(size_t count);
        [[nodiscard]] float* vertices() { return _vertices.data(); }

        /// Segments from pairs of points, one colour each (`colors`, as many as `positions`) or
        /// `color` for all.
        void addLines(const Vector3* positions, size_t count, const Color* colors, const Color& color);
        /// Segments from packed xyz floats (a multiple of six), with packed rgba per vertex or
        /// `color` for all.
        void addLineArrays(const float* positions, size_t floatCount, const float* colors, const Color& color);

        [[nodiscard]] size_t vertexCount() const { return _vertexCount; }
        [[nodiscard]] size_t capacity() const { return _capacity; }

        /// Uploads this frame's vertices into ring slot `slot` and points the mesh at them;
        /// the mesh instance is visible exactly when there is something to draw.
        void upload(size_t slot);

        /// Registers the mesh instance with `layer` (and leaves the previous one), or with none.
        void setLayer(const std::shared_ptr<Layer>& layer);
        [[nodiscard]] const std::shared_ptr<Layer>& layer() const { return _layer; }

        /// Ends the frame: the lines are gone, the storage stays (see the class comment).
        void clear();

        [[nodiscard]] MeshInstance* meshInstance() const { return _meshInstance.get(); }
        [[nodiscard]] Mesh* mesh() const { return _mesh.get(); }

    private:
        void reserve(size_t count);
        void setCapacity(size_t capacity);

        GraphicsDevice* _device = nullptr;
        std::shared_ptr<Material> _material;
        std::shared_ptr<VertexFormat> _format;
        std::shared_ptr<Mesh> _mesh;
        std::unique_ptr<MeshInstance> _meshInstance;
        std::shared_ptr<Layer> _layer;

        std::vector<float> _vertices;
        size_t _vertexCount = 0;
        size_t _capacity = 0;
        size_t _peakVertexCount = 0;
        int _framesSinceHighUse = 0;

        struct RingSlot
        {
            std::shared_ptr<VertexBuffer> buffer;
            size_t capacity = 0;
        };
        std::vector<RingSlot> _ring;
    };
}
