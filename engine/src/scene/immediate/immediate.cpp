// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025
//
#include "immediate.h"

#include <cassert>

#include "immediateBatch.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/composition/layerComposition.h"
#include "scene/graphNode.h"
#include "scene/layer.h"
#include "scene/materials/material.h"
#include "scene/shader-lib/slangShaders.h"

namespace visutwin::canvas
{
    namespace
    {
        // The line program's material: alpha-blended, no culling, and a block that says per
        // draw whether the target takes linear HDR (only known when the draw happens).
        class ImmediateLineMaterial final : public Material
        {
        public:
            ImmediateLineMaterial(const std::shared_ptr<GraphicsDevice>& device, const bool depthTest)
                : _device(device)
            {
                setName(depthTest ? "ImmediateLine" : "ImmediateLineNoDepth");
                setBlendState(std::make_shared<BlendState>(BlendState::alphaBlend()));
                setTransparent(true);
                setCullMode(CullMode::CULLFACE_NONE);
                setDepthState(depthTest ? std::make_shared<DepthState>() : DepthState::noDepth());
                setShaderOverride(getOrCreateSlangShader(device.get(), "immediate-line"));
            }

            const void* customUniformData(size_t& outSize) const override
            {
                _block[0] = _device && _device->hdrPass() ? 1.0f : 0.0f;
                outSize = sizeof(_block);
                return _block;
            }

        private:
            std::shared_ptr<GraphicsDevice> _device;
            mutable float _block[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        };
    }

    Immediate::Immediate(std::shared_ptr<GraphicsDevice> device)
        : _device(std::move(device)), _node(std::make_unique<GraphNode>("Immediate"))
    {
    }

    Immediate::~Immediate()
    {
        // Each batch leaves its layer as it goes.
        _batches.clear();
    }

    std::shared_ptr<Material> Immediate::material(const bool depthTest)
    {
        auto& slot = depthTest ? _materialDepth : _materialNoDepth;
        if (!slot) {
            slot = std::make_shared<ImmediateLineMaterial>(_device, depthTest);
        }
        return slot;
    }

    ImmediateBatch& Immediate::batch(const int layerId, const bool depthTest)
    {
        for (Entry& entry : _batches) {
            if (entry.layerId == layerId && entry.depthTest == depthTest) {
                return *entry.batch;
            }
        }
        Entry entry;
        entry.layerId = layerId;
        entry.depthTest = depthTest;
        entry.batch = std::make_unique<ImmediateBatch>(_device.get(), material(depthTest), _node.get());
        _batches.push_back(std::move(entry));
        return *_batches.back().batch;
    }

    LineWriter& Immediate::allocateLines(const size_t vertexCount, const Color& color, const bool depthTest,
        const int layerId)
    {
        assert(_writer.filled() && "Immediate::allocateLines: the previous allocation was not filled");
        ImmediateBatch& target = batch(layerId, depthTest);
        const size_t first = target.allocate(vertexCount);
        _writer.reset(target.vertices(), first, vertexCount, color);
        return _writer;
    }

    void Immediate::drawLine(const Vector3& start, const Vector3& end, const Color& color, const bool depthTest,
        const int layerId)
    {
        allocateLines(2, color, depthTest, layerId).segment(start, end);
    }

    void Immediate::drawLines(const std::vector<Vector3>& positions, const std::vector<Color>& colors,
        const bool depthTest, const int layerId)
    {
        assert(colors.size() == positions.size() && "Immediate::drawLines: one colour per position");
        const size_t count = positions.size() & ~size_t{1};
        batch(layerId, depthTest).addLines(positions.data(), count,
            colors.size() >= count ? colors.data() : nullptr, Color(1.0f, 1.0f, 1.0f, 1.0f));
    }

    void Immediate::drawLines(const std::vector<Vector3>& positions, const Color& color, const bool depthTest,
        const int layerId)
    {
        batch(layerId, depthTest).addLines(positions.data(), positions.size() & ~size_t{1}, nullptr, color);
    }

    void Immediate::drawLineArrays(const float* positions, const size_t floatCount, const float* colors,
        const Color& color, const bool depthTest, const int layerId)
    {
        if (!positions) {
            return;
        }
        batch(layerId, depthTest).addLineArrays(positions, floatCount - floatCount % 6, colors, color);
    }

    void Immediate::prepareFrame(const LayerComposition* layers)
    {
        // Each batch has a ring slot per frame in flight, and this runs once a frame, so a slot
        // is written again only once the frame that read it last has finished.
        const size_t slot = _frame++;
        for (Entry& entry : _batches) {
            if (entry.batch->vertexCount() > 0) {
                entry.batch->setLayer(layers ? layers->getLayerById(entry.layerId) : nullptr);
            }
            entry.batch->upload(slot);
        }
    }

    void Immediate::clear()
    {
        for (Entry& entry : _batches) {
            entry.batch->clear();
        }
    }
}
