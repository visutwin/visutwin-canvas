// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/rangeAllocator.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class IndexBuffer;
    class VertexBuffer;
    class VertexFormat;

    /**
     * The vertex and index buffers every UI visual's geometry lives in.
     *
     * An element's quad (or its sliced grid, or a run of glyphs) is a run of vertices
     * and a run of indices inside a few large shared buffers, not a pair of buffers
     * of its own. Two things follow. Consecutive UI draws bind the same buffers, where
     * a buffer pair per element makes every draw a new vertex and index binding and a
     * new entry in the driver's resource tracking. And an element that changes size
     * takes a new run and gives the old one back: no GPU buffer is created or
     * destroyed, however often it is resized.
     *
     * A run that was drawn is not handed out again until `maxFramesInFlight` frames
     * have begun since it was given back: on Metal a write lands in memory the GPU
     * reads directly, and a frame still in flight may be drawing the old contents.
     * beginFrame() is the clock, once per rendered frame.
     *
     * Indices are stored ABSOLUTE (the block's first vertex already added), 32 bits
     * wide, so a draw needs only the block's first index and count.
     */
    class UiGeometryArena : public std::enable_shared_from_this<UiGeometryArena>
    {
    public:
        /// One piece of geometry in the arena. Its destructor gives the runs back.
        class Block
        {
        public:
            ~Block();
            Block(const Block&) = delete;
            Block& operator=(const Block&) = delete;

            /// Null when the device creates no buffers (a test device).
            [[nodiscard]] const std::shared_ptr<VertexBuffer>& vertexBuffer() const { return _vertexBuffer; }
            [[nodiscard]] const std::shared_ptr<IndexBuffer>& indexBuffer() const { return _indexBuffer; }
            [[nodiscard]] uint32_t firstVertex() const { return _firstVertex; }
            [[nodiscard]] uint32_t vertexCount() const { return _vertexCount; }
            [[nodiscard]] uint32_t firstIndex() const { return _firstIndex; }
            [[nodiscard]] uint32_t indexCount() const { return _indexCount; }

        private:
            friend class UiGeometryArena;
            Block() = default;

            std::weak_ptr<UiGeometryArena> _arena;
            uint32_t _chunkId = 0;
            std::shared_ptr<VertexBuffer> _vertexBuffer;
            std::shared_ptr<IndexBuffer> _indexBuffer;
            uint32_t _firstVertex = 0;
            uint32_t _vertexCount = 0;
            uint32_t _firstIndex = 0;
            uint32_t _indexCount = 0;
        };

        /// Floats per vertex: position(3) normal(3) uv0(2) tangent(4) uv1(2), the
        /// engine's standard packed layout.
        static constexpr uint32_t kFloatsPerVertex = 14;
        /// The first chunk's size in vertices; each further chunk doubles, up to the
        /// maximum. A single block larger than the next chunk gets a chunk of its own.
        static constexpr uint32_t kFirstChunkVertices = 2048;
        static constexpr uint32_t kMaxChunkVertices = 65536;

        static std::shared_ptr<UiGeometryArena> create(GraphicsDevice* device);

        /// Stores the geometry and returns its block, or null when there is nothing to
        /// store. `vertices` holds kFloatsPerVertex floats per vertex; `indices` are
        /// relative to the block's own first vertex.
        std::unique_ptr<Block> allocate(const std::vector<float>& vertices, const std::vector<uint32_t>& indices);

        /// A new frame: runs given back `maxFramesInFlight` frames ago become free.
        void beginFrame();

        [[nodiscard]] size_t chunkCount() const { return _chunks.size(); }
        [[nodiscard]] const std::shared_ptr<VertexFormat>& vertexFormat() const { return _format; }

    private:
        explicit UiGeometryArena(GraphicsDevice* device);

        struct Chunk
        {
            uint32_t id = 0;
            std::shared_ptr<VertexBuffer> vertexBuffer;
            std::shared_ptr<IndexBuffer> indexBuffer;
            RangeAllocator vertices;
            RangeAllocator indices;
            // Runs given back and not yet free.
            uint32_t pending = 0;
        };

        struct PendingRelease
        {
            uint64_t frame;
            uint32_t chunkId;
            uint32_t firstVertex;
            uint32_t vertexCount;
            uint32_t firstIndex;
            uint32_t indexCount;
        };

        Chunk* findChunk(uint32_t id);
        Chunk& addChunk(uint32_t vertexCount, uint32_t indexCount);
        void release(const Block& block);

        GraphicsDevice* _device;
        std::shared_ptr<VertexFormat> _format;
        std::vector<std::unique_ptr<Chunk>> _chunks;
        // In the order they were given back, so the oldest is first.
        std::vector<PendingRelease> _pending;
        uint64_t _frame = 0;
        uint32_t _reuseDelay = 1;
        uint32_t _nextChunkId = 1;
        uint32_t _nextChunkVertices = kFirstChunkVertices;
    };
}
