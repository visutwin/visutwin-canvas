// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2026.
//
#include "uiGeometryArena.h"

#include <algorithm>
#include <cstddef>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/indexBuffer.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"

namespace visutwin::canvas
{
    namespace
    {
        constexpr size_t kVertexBytes = UiGeometryArena::kFloatsPerVertex * sizeof(float);

        // UI geometry is quads: six indices for every four vertices.
        uint32_t indicesFor(const uint32_t vertexCount)
        {
            return vertexCount / 4u * 6u;
        }
    }

    UiGeometryArena::Block::~Block()
    {
        if (const auto arena = _arena.lock()) {
            arena->release(*this);
        }
    }

    std::shared_ptr<UiGeometryArena> UiGeometryArena::create(GraphicsDevice* device)
    {
        return std::shared_ptr<UiGeometryArena>(new UiGeometryArena(device));
    }

    UiGeometryArena::UiGeometryArena(GraphicsDevice* device)
        : _device(device),
          _format(std::make_shared<VertexFormat>(static_cast<int>(kVertexBytes), VertexFormat::standardElements(),
              true, false)),
          _reuseDelay(static_cast<uint32_t>(std::max(device ? device->maxFramesInFlight() : 1, 1)))
    {
    }

    UiGeometryArena::Chunk* UiGeometryArena::findChunk(const uint32_t id)
    {
        for (const auto& chunk : _chunks) {
            if (chunk->id == id) {
                return chunk.get();
            }
        }
        return nullptr;
    }

    UiGeometryArena::Chunk& UiGeometryArena::addChunk(const uint32_t vertexCount, const uint32_t indexCount)
    {
        auto chunk = std::make_unique<Chunk>(Chunk{_nextChunkId++, nullptr, nullptr, RangeAllocator(vertexCount),
                                                   RangeAllocator(indexCount), 0});
        if (_device) {
            // Created zeroed; every block writes its own run.
            chunk->vertexBuffer = _device->createVertexBuffer(_format, static_cast<int>(vertexCount),
                VertexBufferOptions{});
            chunk->indexBuffer = _device->createIndexBuffer(INDEXFORMAT_UINT32, static_cast<int>(indexCount),
                std::vector<uint8_t>(static_cast<size_t>(indexCount) * sizeof(uint32_t), 0));
        }
        _chunks.push_back(std::move(chunk));
        return *_chunks.back();
    }

    std::unique_ptr<UiGeometryArena::Block> UiGeometryArena::allocate(const std::vector<float>& vertices,
                                                                      const std::vector<uint32_t>& indices)
    {
        const auto vertexCount = static_cast<uint32_t>(vertices.size() / kFloatsPerVertex);
        const auto indexCount = static_cast<uint32_t>(indices.size());
        if (vertexCount == 0 || indexCount == 0) {
            return nullptr;
        }

        Chunk* home = nullptr;
        uint32_t firstVertex = 0;
        uint32_t firstIndex = 0;
        for (const auto& chunk : _chunks) {
            const auto vertexStart = chunk->vertices.allocate(vertexCount);
            if (!vertexStart) {
                continue;
            }
            const auto indexStart = chunk->indices.allocate(indexCount);
            if (!indexStart) {
                chunk->vertices.release(*vertexStart, vertexCount);
                continue;
            }
            home = chunk.get();
            firstVertex = *vertexStart;
            firstIndex = *indexStart;
            break;
        }
        if (!home) {
            const uint32_t chunkVertices = std::max(_nextChunkVertices, vertexCount);
            const uint32_t chunkIndices = std::max(indicesFor(chunkVertices), indexCount);
            _nextChunkVertices = std::min(_nextChunkVertices * 2u, kMaxChunkVertices);
            home = &addChunk(chunkVertices, chunkIndices);
            firstVertex = *home->vertices.allocate(vertexCount);
            firstIndex = *home->indices.allocate(indexCount);
        }

        if (home->vertexBuffer) {
            home->vertexBuffer->writeRange(static_cast<size_t>(firstVertex) * kVertexBytes, vertices.data(),
                static_cast<size_t>(vertexCount) * kVertexBytes);
        }
        if (home->indexBuffer) {
            std::vector<uint32_t> absolute(indices.size());
            for (size_t i = 0; i < indices.size(); ++i) {
                absolute[i] = indices[i] + firstVertex;
            }
            home->indexBuffer->writeRange(static_cast<size_t>(firstIndex) * sizeof(uint32_t), absolute.data(),
                absolute.size() * sizeof(uint32_t));
        }

        std::unique_ptr<Block> block(new Block());
        block->_arena = weak_from_this();
        block->_chunkId = home->id;
        block->_vertexBuffer = home->vertexBuffer;
        block->_indexBuffer = home->indexBuffer;
        block->_firstVertex = firstVertex;
        block->_vertexCount = vertexCount;
        block->_firstIndex = firstIndex;
        block->_indexCount = indexCount;
        return block;
    }

    void UiGeometryArena::release(const Block& block)
    {
        if (Chunk* chunk = findChunk(block._chunkId)) {
            ++chunk->pending;
            _pending.push_back({_frame, block._chunkId, block._firstVertex, block._vertexCount, block._firstIndex,
                                block._indexCount});
        }
    }

    void UiGeometryArena::beginFrame()
    {
        ++_frame;
        size_t done = 0;
        while (done < _pending.size() && _pending[done].frame + _reuseDelay <= _frame) {
            const PendingRelease& entry = _pending[done++];
            if (Chunk* chunk = findChunk(entry.chunkId)) {
                chunk->vertices.release(entry.firstVertex, entry.vertexCount);
                chunk->indices.release(entry.firstIndex, entry.indexCount);
                --chunk->pending;
            }
        }
        if (done == 0) {
            return;
        }
        _pending.erase(_pending.begin(), _pending.begin() + static_cast<std::ptrdiff_t>(done));

        // A chunk nothing lives in any more is dropped, the first one excepted: a UI
        // that empties and refills should not create its buffers again each time.
        for (size_t i = _chunks.size(); i-- > 1;) {
            const Chunk& chunk = *_chunks[i];
            if (chunk.pending == 0 && chunk.vertices.allFree() && chunk.indices.allFree()) {
                _chunks.erase(_chunks.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }
}
