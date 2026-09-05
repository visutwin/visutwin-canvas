// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Draws a collection of WideLines (upstream extras/renderers/wide-line-renderer.js).
#pragma once

#include <memory>
#include <vector>

#include "core/math/matrix4.h"
#include "scene/graphics/wideLine.h"

namespace visutwin::canvas
{
    class Engine;
    class Entity;
    class GraphicsDevice;
    class Mesh;
    class ShaderMaterial;
    class VertexBuffer;

    /// Whether a line's width is a count of screen pixels or a world-space size.
    enum class LineWidthUnits { Pixels, World };

    /**
     * Renders every WideLine it owns in one instanced draw: one instance per
     * SEGMENT, expanded to a quad plus caps and joins in the vertex shader, so
     * lines of different widths, colours, caps, joins and dash patterns stay in the
     * same batch.
     *
     * The renderer owns an entity in the scene. Add lines, then call `update()`
     * whenever their data changes — nothing is uploaded until then, so building a
     * line point by point costs nothing per point.
     */
    class WideLineRenderer
    {
    public:
        WideLineRenderer(Engine* engine, std::shared_ptr<GraphicsDevice> device);
        ~WideLineRenderer();

        void add(WideLine* line);
        void remove(WideLine* line);
        [[nodiscard]] const std::vector<WideLine*>& lines() const { return _lines; }

        LineWidthUnits widthUnits() const { return _widthUnits; }
        void setWidthUnits(const LineWidthUnits value) { _widthUnits = value; _dirty = true; }

        /// The render target size in pixels, needed to expand a screen-space width.
        void setScreenSize(float width, float height);

        /// Rebuilds the instance data from every line that changed. Cheap when
        /// nothing did.
        void update();

    private:
        void rebuild();

        Engine* _engine = nullptr;
        std::shared_ptr<GraphicsDevice> _device;
        std::vector<WideLine*> _lines;

        Entity* _entity = nullptr;
        std::shared_ptr<Mesh> _mesh;
        std::shared_ptr<ShaderMaterial> _material;
        std::shared_ptr<VertexBuffer> _segmentBuffer;
        int _segmentCapacity = 0;

        struct LineParams { float screenSize[4] = {1.0f, 1.0f, 1.0f, 1.0f}; };
        LineParams _params;

        LineWidthUnits _widthUnits = LineWidthUnits::Pixels;
        bool _dirty = true;
    };
}
