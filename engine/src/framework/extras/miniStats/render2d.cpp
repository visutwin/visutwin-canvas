// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 04.10.2026
//
#include "render2d.h"

#include <algorithm>
#include <limits>
#include <utility>

#include <spdlog/spdlog.h>

#include "core/shape/boundingBox.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/shader-lib/slangShaders.h"
#include "scene/graphNode.h"
#include "scene/layer.h"
#include "scene/mesh.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    Render2dMaterial::Render2dMaterial(const std::shared_ptr<GraphicsDevice>& device)
        : ShaderMaterial("render2d", getOrCreateSlangShader(device.get(), "render2d"))
    {
        // Alpha mode first: it resets the blend and depth state set after it.
        setAlphaMode(AlphaMode::BLEND);
        auto blend = std::make_shared<BlendState>(BlendState::alphaBlend());
        // Straight alpha into colour, and alpha accumulated as coverage.
        blend->setAlphaSrcFactor(BLENDMODE_ONE);
        blend->setAlphaDstFactor(BLENDMODE_ONE_MINUS_SRC_ALPHA);
        setBlendState(blend);
        setDepthState(DepthState::noDepth());
        setCullMode(CullMode::CULLFACE_NONE);
    }

    void Render2dMaterial::setTextures(const Textures& textures)
    {
        if (textures == _textures) {
            return;
        }
        _textures = textures;
        // A texture setter must move the version: both backends skip rebinding the
        // textures of a material whose version they have already seen.
        markUniformsDirty();
    }

    void Render2dMaterial::setUniforms(const std::array<float, 4>& clr, const std::array<float, 4>& params,
                                       const std::array<float, 4>& pages)
    {
        if (clr == _data.clr && params == _data.params && pages == _data.pages) {
            return;
        }
        _data.clr = clr;
        _data.params = params;
        _data.pages = pages;
        markUniformsDirty();
    }

    void Render2dMaterial::getTextureSlots(std::vector<TextureSlot>& slots) const
    {
        slots.clear();
        const std::pair<int, Texture*> bound[] = {
            {kRegularPage0Slot, _textures.regularPage0}, {kBoldPage0Slot, _textures.boldPage0},
            {kGraphSlot, _textures.graph}, {kRegularPage1Slot, _textures.regularPage1},
            {kBoldPage1Slot, _textures.boldPage1}};
        for (const auto& [slot, texture] : bound) {
            if (texture) {
                slots.push_back({slot, texture});
            }
        }
    }

    const void* Render2dMaterial::customUniformData(size_t& outSize) const
    {
        outSize = sizeof(_data);
        return &_data;
    }

    Render2d::Render2d(const std::shared_ptr<GraphicsDevice>& device)
        : _device(device)
    {
        _arena = UiGeometryArena::create(device.get());
        _mesh = std::make_shared<Mesh>();
        _material = std::make_shared<Render2dMaterial>(device);
        _node = std::make_unique<GraphNode>("Render2d");
        _meshInstance = std::make_unique<MeshInstance>(_mesh.get(), _material.get(), _node.get());
        _meshInstance->setScreenSpace(true);
        _meshInstance->setReceiveShadow(false);
        _meshInstance->setDrawOncePerFrame(true);
        // After every other draw of a manually sorted (UI) layer.
        _meshInstance->setDrawOrder(std::numeric_limits<double>::infinity());
        _meshInstance->setVisible(false);
    }

    Render2d::~Render2d()
    {
        setLayer(nullptr);
    }

    void Render2d::setTargetSize(const float width, const float height)
    {
        const float w = std::max(width, 1.0f);
        const float h = std::max(height, 1.0f);
        if (w != _targetWidth || h != _targetHeight) {
            _targetWidth = w;
            _targetHeight = h;
            // Vertices are stored in fractions of the target.
            _dirty = true;
        }
    }

    void Render2d::setClip(const float x, const float y, const float width, const float height)
    {
        _clipLeft = x;
        _clipBottom = y;
        _clipRight = x + width;
        _clipTop = y + height;
        _clipped = true;
    }

    void Render2d::clearClip()
    {
        _clipped = false;
    }

    void Render2d::startFrame()
    {
        _quads.clear();
        _dirty = true;
    }

    int Render2d::quad(const float x, const float y, const float width, const float height,
                       const float u, const float v, const float uw, const float uh,
                       const float textureWidth, const float textureHeight, const Mode mode, const uint32_t color)
    {
        if (width <= 0.0f || height <= 0.0f) {
            return -1;
        }
        float x0 = x;
        float y0 = y;
        float x1 = x + width;
        float y1 = y + height;
        if (_clipped) {
            x0 = std::max(x0, _clipLeft);
            y0 = std::max(y0, _clipBottom);
            x1 = std::min(x1, _clipRight);
            y1 = std::min(y1, _clipTop);
        }
        if (x1 <= x0 || y1 <= y0) {
            return -1;
        }
        if (static_cast<int>(_quads.size()) >= kMaxQuads) {
            if (!_warnedFull) {
                _warnedFull = true;
                spdlog::warn("Render2d: more than {} quads; the rest are dropped", kMaxQuads);
            }
            return -1;
        }

        const float tw = textureWidth > 0.0f ? textureWidth : 1.0f;
        const float th = textureHeight > 0.0f ? textureHeight : 1.0f;
        const float left = (x0 - x) / width;
        const float right = (x1 - x) / width;
        const float bottom = (y0 - y) / height;
        const float top = (y1 - y) / height;

        Quad q{};
        q.x0 = x0;
        q.y0 = y0;
        q.x1 = x1;
        q.y1 = y1;
        q.u0 = (u + left * uw) / tw;
        q.u1 = (u + right * uw) / tw;
        // The source rectangle's top row is v; the quad's bottom edge samples its bottom.
        q.v0 = (v + (1.0f - bottom) * uh) / th;
        q.v1 = (v + (1.0f - top) * uh) / th;
        q.bottom = bottom;
        q.top = top;
        q.invHeight = 1.0f / height;
        q.mode = mode;
        q.color = color;
        _quads.push_back(q);
        _dirty = true;
        return static_cast<int>(_quads.size()) - 1;
    }

    int Render2d::rect(const float x, const float y, const float width, const float height, const uint32_t color)
    {
        return quad(x, y, width, height, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, Mode::Solid, color);
    }

    int Render2d::graph(const float x, const float y, const float width, const float height, const int row,
                        const float textureWidth, const float textureHeight, const uint32_t color)
    {
        // Columns [cursor - width, cursor), the cursor added by the shader.
        return quad(x, y, width, height, -width, static_cast<float>(row) + 0.5f, width, 0.0f,
                    textureWidth, textureHeight, Mode::Graph, color);
    }

    void Render2d::setTextures(const Render2dMaterial::Textures& textures)
    {
        _material->setTextures(textures);
    }

    void Render2d::setMsdf(const float pixelRange, const float page0Width, const float page0Height,
                           const float page1Width, const float page1Height)
    {
        _params[1] = pixelRange;
        _params[2] = page0Width;
        _params[3] = page0Height;
        _pages[0] = page1Width;
        _pages[1] = page1Height;
    }

    void Render2d::setGraphCursor(const float texels, const float textureWidth)
    {
        _params[0] = textureWidth > 0.0f ? texels / textureWidth : 0.0f;
    }

    void Render2d::setColor(const float r, const float g, const float b, const float a)
    {
        _clr = {r, g, b, a};
    }

    void Render2d::commit()
    {
        _dirty = false;
        _vertices.clear();
        _indices.clear();
        _vertices.reserve(_quads.size() * 4 * UiGeometryArena::kFloatsPerVertex);
        _indices.reserve(_quads.size() * 6);

        const float invW = 1.0f / _targetWidth;
        const float invH = 1.0f / _targetHeight;
        for (const Quad& q : _quads) {
            const auto base = static_cast<uint32_t>(_vertices.size() / UiGeometryArena::kFloatsPerVertex);
            const float r = static_cast<float>(q.color & 0xffu) / 255.0f;
            const float g = static_cast<float>((q.color >> 8) & 0xffu) / 255.0f;
            const float b = static_cast<float>((q.color >> 16) & 0xffu) / 255.0f;
            const float a = static_cast<float>((q.color >> 24) & 0xffu) / 255.0f;
            const float mode = static_cast<float>(q.mode);
            // Bottom-left, bottom-right, top-right, top-left.
            for (int i = 0; i < 4; ++i) {
                const bool right = i == 1 || i == 2;
                const bool upper = i >= 2;
                const float vertex[UiGeometryArena::kFloatsPerVertex] = {
                    (right ? q.x1 : q.x0) * invW, (upper ? q.y1 : q.y0) * invH, mode,   // position
                    r, g, b,                                                            // normal
                    right ? q.u1 : q.u0, upper ? q.v1 : q.v0,                           // uv0
                    q.invHeight, upper ? q.top : q.bottom, a, 0.0f,                     // tangent
                    0.0f, 0.0f                                                          // uv1
                };
                _vertices.insert(_vertices.end(), std::begin(vertex), std::end(vertex));
            }
            for (const uint32_t index : {0u, 1u, 2u, 0u, 2u, 3u}) {
                _indices.push_back(base + index);
            }
        }

        // The block this replaces goes back to the arena, which keeps it out of use until
        // no frame in flight can still be drawing it.
        _block = _quads.empty() ? nullptr : _arena->allocate(_vertices, _indices);
        if (!_block) {
            _meshInstance->setVisible(false);
            return;
        }
        _mesh->setVertexBuffer(_block->vertexBuffer());
        _mesh->setIndexBuffer(_block->indexBuffer(), 0);
        Primitive primitive;
        primitive.type = PRIMITIVE_TRIANGLES;
        primitive.base = static_cast<int>(_block->firstIndex());
        primitive.count = static_cast<int>(_block->indexCount());
        primitive.indexed = true;
        _mesh->setPrimitive(primitive, 0);
        _mesh->setAabb(BoundingBox(Vector3(0.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f)));
        _meshInstance->setVisible(true);
    }

    void Render2d::render(Layer* layer)
    {
        _arena->beginFrame();
        if (_dirty) {
            commit();
        }
        _material->setUniforms(_clr, _params, _pages);
        setLayer(layer);
    }

    void Render2d::setLayer(Layer* layer)
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
}
