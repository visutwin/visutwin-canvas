// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.10.2025
//
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "lineWriter.h"
#include "core/math/color.h"
#include "core/math/vector3.h"
#include "scene/constants.h"

namespace visutwin::canvas
{
    class GraphicsDevice;
    class GraphNode;
    class ImmediateBatch;
    class LayerComposition;
    class Material;

    /**
     * Lines drawn for ONE frame, for debugging and visualisation (the scene's `drawLine`
     * family and WireRenderer). Lines go into a batch per (layer, depth test), are uploaded
     * when the frame starts (`prepareFrame`, called by the engine after the `prerender` event)
     * and discarded once it has been rendered (`clear`, after `postrender`), so they must be
     * issued again every frame they should be seen. Colours are gamma encoded, as material
     * colours are; a line is alpha-blended in its layer's transparent sublayer and drawn by
     * every camera that renders the layer.
     */
    class Immediate
    {
    public:
        explicit Immediate(std::shared_ptr<GraphicsDevice> device);
        ~Immediate();

        Immediate(const Immediate&) = delete;
        Immediate& operator=(const Immediate&) = delete;

        /**
         * Room for `vertexCount` vertices (two per segment) in the batch for `layerId` and
         * `depthTest`, as a cursor that writes `color` until told otherwise. The cursor is the
         * Immediate's one LineWriter: fill it before the next allocation.
         */
        LineWriter& allocateLines(size_t vertexCount, const Color& color, bool depthTest = true,
            int layerId = LAYERID_IMMEDIATE);

        void drawLine(const Vector3& start, const Vector3& end, const Color& color = Color(1.0f, 1.0f, 1.0f, 1.0f),
            bool depthTest = true, int layerId = LAYERID_IMMEDIATE);
        /// Separate segments from pairs of points, with a colour per point.
        void drawLines(const std::vector<Vector3>& positions, const std::vector<Color>& colors,
            bool depthTest = true, int layerId = LAYERID_IMMEDIATE);
        /// Separate segments from pairs of points, all in one colour.
        void drawLines(const std::vector<Vector3>& positions, const Color& color,
            bool depthTest = true, int layerId = LAYERID_IMMEDIATE);
        /// Separate segments from packed xyz floats (`floatCount` a multiple of six), with
        /// packed rgba per vertex (`colors`, may be null to use `color`).
        void drawLineArrays(const float* positions, size_t floatCount, const float* colors, const Color& color,
            bool depthTest = true, int layerId = LAYERID_IMMEDIATE);

        /// Uploads every batch drawn into this frame and registers it with its layer.
        void prepareFrame(const LayerComposition* layers);
        /// Ends the frame: every line is gone.
        void clear();

        /// The batch for a layer and depth-test mode, made on first use.
        ImmediateBatch& batch(int layerId, bool depthTest);
        [[nodiscard]] size_t batchCount() const { return _batches.size(); }

    private:
        std::shared_ptr<Material> material(bool depthTest);

        std::shared_ptr<GraphicsDevice> _device;
        std::unique_ptr<GraphNode> _node;
        std::shared_ptr<Material> _materialDepth;
        std::shared_ptr<Material> _materialNoDepth;

        struct Entry
        {
            int layerId = 0;
            bool depthTest = true;
            std::unique_ptr<ImmediateBatch> batch;
        };
        std::vector<Entry> _batches;
        LineWriter _writer;
        size_t _frame = 0;
    };
}
