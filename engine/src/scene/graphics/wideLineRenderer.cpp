// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "wideLineRenderer.h"

#include <cmath>
#include <cstring>

#include <spdlog/spdlog.h>

#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/components/componentSystem.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/vertexBuffer.h"
#include "platform/graphics/vertexFormat.h"
#include "scene/graphics/wideLineShaders.h"
#include "scene/materials/shaderMaterial.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        /// One instance record, matching the `Segment` struct in both shaders.
        struct SegmentRecord
        {
            float prevWidth[4];     // prev.xyz,  startWidth
            float startWidth[4];    // start.xyz, endWidth
            float endDistance[4];   // end.xyz,   startDistance
            float nextDistance[4];  // next.xyz,  endDistance
            float startColor[4];
            float endColor[4];
            float style[4];         // join, cap, dashLength, gapLength
            float dashFlags[4];     // dashOffset, connectedFlags, worldSpaceWidth, pad
        };

        void readPoint(const WideLine& line, const size_t index, float* out)
        {
            const auto& p = line.positions();
            const size_t clamped = std::min(index, line.pointCount() - 1);
            out[0] = p[clamped * 3 + 0];
            out[1] = p[clamped * 3 + 1];
            out[2] = p[clamped * 3 + 2];
        }

        void readColor(const WideLine& line, const size_t index, float* out)
        {
            const auto& c = line.colors();
            // One colour for the whole line, or one per point.
            const size_t base = c.size() == 3 ? 0 : std::min(index, line.pointCount() - 1) * 3;
            out[0] = c[base + 0];
            out[1] = c[base + 1];
            out[2] = c[base + 2];
            out[3] = 1.0f;
        }

        float readWidth(const WideLine& line, const size_t index)
        {
            const auto& w = line.widths();
            return w.size() == 1 ? w[0] : w[std::min(index, line.pointCount() - 1)];
        }

        float distanceBetween(const float* a, const float* b)
        {
            const float dx = b[0] - a[0];
            const float dy = b[1] - a[1];
            const float dz = b[2] - a[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    WideLineRenderer::WideLineRenderer(Engine* engine, std::shared_ptr<GraphicsDevice> device)
        : _engine(engine), _device(std::move(device))
    {
        if (_engine == nullptr || _device == nullptr) {
            return;
        }

        _material = std::make_shared<ShaderMaterial>(_device, "WideLineShader",
            "wideLineVS", "wideLineFS",
            ShaderSourceSet{.msl = wideline::WIDE_LINE_MSL, .glsl = wideline::WIDE_LINE_GLSL});
        // A screen-space expansion has no meaningful winding, and both faces of a
        // dashed line have to survive.
        _material->setCullMode(CullMode::CULLFACE_NONE);

        // The template geometry comes from the vertex id, so the buffer exists only
        // to give the draw its vertex count.
        auto format = std::make_shared<VertexFormat>(
            14 * static_cast<int>(sizeof(float)), VertexFormat::standardElements(), true, false);
        VertexBufferOptions options;
        options.data.assign(
            static_cast<size_t>(wideline::kTemplateVertices) * 14 * sizeof(float), 0);
        auto buffer = _device->createVertexBuffer(format, wideline::kTemplateVertices, options);

        _mesh = std::make_shared<Mesh>();
        _mesh->setVertexBuffer(buffer);
        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = 0;
        primitive.count = wideline::kTemplateVertices;
        primitive.indexed = false;
        _mesh->setPrimitive(primitive, 0);

        _entity = new Entity();
        _entity->setName("WideLineRenderer");
        _entity->setEngine(_engine);
        _engine->root()->addChild(_entity);
    }

    WideLineRenderer::~WideLineRenderer()
    {
        if (_entity != nullptr && _entity->parent() != nullptr) {
            const auto owned = _entity->parent()->removeChild(_entity);
        }
    }

    void WideLineRenderer::add(WideLine* line)
    {
        if (line == nullptr ||
            std::find(_lines.begin(), _lines.end(), line) != _lines.end()) {
            return;
        }
        _lines.push_back(line);
        _dirty = true;
    }

    void WideLineRenderer::remove(WideLine* line)
    {
        const auto it = std::find(_lines.begin(), _lines.end(), line);
        if (it != _lines.end()) {
            _lines.erase(it);
            _dirty = true;
        }
    }

    void WideLineRenderer::setScreenSize(const float width, const float height)
    {
        _params.screenSize[0] = width;
        _params.screenSize[1] = height;
        _params.screenSize[2] = width > 0.0f ? 1.0f / width : 1.0f;
        _params.screenSize[3] = height > 0.0f ? 1.0f / height : 1.0f;
    }

    void WideLineRenderer::update()
    {
        bool needsRebuild = _dirty;
        for (WideLine* line : _lines) {
            if (line != nullptr && line->dirty()) {
                needsRebuild = true;
            }
        }
        if (needsRebuild) {
            rebuild();
        }
    }

    void WideLineRenderer::rebuild()
    {
        std::vector<SegmentRecord> records;
        for (WideLine* line : _lines) {
            if (line == nullptr || line->pointCount() < 2) {
                continue;
            }
            const size_t points = line->pointCount();
            const size_t segments = line->segmentCount();
            const bool closed = line->closed();

            // Distance travelled along the line, so a dash pattern is continuous
            // across segments rather than restarting at each one.
            float travelled = 0.0f;
            for (size_t i = 0; i < segments; ++i) {
                const size_t startIndex = i;
                const size_t endIndex = (i + 1) % points;

                SegmentRecord record{};
                float start[3];
                float end[3];
                readPoint(*line, startIndex, start);
                readPoint(*line, endIndex, end);

                // The neighbours the shader needs for its joins. On an open line the
                // ends have no neighbour, so they repeat the segment's own point and
                // the shader falls back to the segment direction.
                const bool startConnected = closed || i > 0;
                const bool endConnected = closed || i + 1 < segments;
                float prev[3];
                float next[3];
                readPoint(*line, startConnected ? (startIndex + points - 1) % points : startIndex, prev);
                readPoint(*line, endConnected ? (endIndex + 1) % points : endIndex, next);

                const float segmentLength = distanceBetween(start, end);

                record.prevWidth[0] = prev[0];
                record.prevWidth[1] = prev[1];
                record.prevWidth[2] = prev[2];
                record.prevWidth[3] = readWidth(*line, startIndex);

                record.startWidth[0] = start[0];
                record.startWidth[1] = start[1];
                record.startWidth[2] = start[2];
                record.startWidth[3] = readWidth(*line, endIndex);

                record.endDistance[0] = end[0];
                record.endDistance[1] = end[1];
                record.endDistance[2] = end[2];
                record.endDistance[3] = travelled;

                record.nextDistance[0] = next[0];
                record.nextDistance[1] = next[1];
                record.nextDistance[2] = next[2];
                record.nextDistance[3] = travelled + segmentLength;

                readColor(*line, startIndex, record.startColor);
                readColor(*line, endIndex, record.endColor);

                record.style[0] = static_cast<float>(line->join());
                record.style[1] = static_cast<float>(line->cap());
                record.style[2] = line->dashLength();
                record.style[3] = line->gapLength();

                record.dashFlags[0] = line->dashOffset();
                record.dashFlags[1] = static_cast<float>(
                    (startConnected ? 1u : 0u) | (endConnected ? 2u : 0u));
                record.dashFlags[2] = _widthUnits == LineWidthUnits::World ? 1.0f : 0.0f;

                records.push_back(record);
                travelled += segmentLength;
            }
            line->clearDirty();
        }
        _dirty = false;

        auto* render = _entity->findComponent<RenderComponent>();
        if (records.empty()) {
            if (render != nullptr) {
                render->clearMeshInstances();
            }
            return;
        }

        // The storage buffer is recreated only when it has to grow, so a line whose
        // points move every frame does not reallocate.
        const int count = static_cast<int>(records.size());
        if (_segmentBuffer == nullptr || count > _segmentCapacity) {
            auto format = std::make_shared<VertexFormat>(
                static_cast<int>(sizeof(SegmentRecord)), true, false);
            VertexBufferOptions options;
            options.usage = BUFFER_DYNAMIC;
            options.data.resize(records.size() * sizeof(SegmentRecord));
            std::memcpy(options.data.data(), records.data(), options.data.size());
            _segmentBuffer = _device->createVertexBuffer(format, count, options);
            _segmentCapacity = count;
        } else {
            std::vector<uint8_t> bytes(records.size() * sizeof(SegmentRecord));
            std::memcpy(bytes.data(), records.data(), bytes.size());
            _segmentBuffer->setData(bytes);
        }

        if (render == nullptr) {
            render = static_cast<RenderComponent*>(_entity->addComponent<RenderComponent>());
        }
        if (render == nullptr) {
            spdlog::error("WideLineRenderer: the host entity refused a render component");
            return;
        }

        render->clearMeshInstances();
        auto instance = std::make_unique<MeshInstance>(_mesh, _material, _entity);
        instance->setStorageDraw(_segmentBuffer, count, &_params, sizeof(_params));
        // Lines are given in world space and reach wherever their points do.
        instance->setCull(false);
        instance->setCastShadow(false);
        render->addMeshInstance(std::move(instance));
    }
}
