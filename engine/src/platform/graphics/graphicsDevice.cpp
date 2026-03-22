// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#include <cmath>
#include <cstring>

#include "graphicsDevice.h"

namespace visutwin::canvas
{
    namespace
    {
        struct QuadVertex
        {
            float position[3];
            float normal[3];
            float uv0[2];
            float tangent[4];
            float uv1[2];
        };
    }

    GraphicsDevice::~GraphicsDevice() {
        // Clean up resources
        if (_quadVertexBuffer) {
            _quadVertexBuffer.reset();
        }

        if (_dynamicBuffers) {
            _dynamicBuffers.reset();
        }

        if (_gpuProfiler) {
            _gpuProfiler.reset();
        }
    }

    void GraphicsDevice::frameStart()
    {
        _renderPassIndex = 0;
        _renderVersion++;
        onFrameStart();
    }

    void GraphicsDevice::frameEnd()
    {
        // Clear all maps scheduled for end-of-frame clearing
        for (auto* map : _mapsToClear) {
            map->clear();
        }
        _mapsToClear.clear();
        onFrameEnd();
    }

    std::shared_ptr<Shader> GraphicsDevice::createShader(const ShaderDefinition& definition,
        const std::string& sourceCode)
    {
        (void)sourceCode;
        return std::make_shared<Shader>(this, definition);
    }

    void GraphicsDevice::clearVertexBuffer()
    {
        _vertexBuffers.clear();
    }

    void GraphicsDevice::resizeCanvas(int width, int height) {
        float pixelRatio = std::min(_maxPixelRatio, 1.0f); // Would get actual device pixel ratio
        int w = static_cast<int>(std::floor(width * pixelRatio));
        int h = static_cast<int>(std::floor(height * pixelRatio));

        auto size = this->size();
        if (w != size.first || h != size.second) {
            setResolution(w, h);
        }
    }

    void GraphicsDevice::update()
    {
        updateClientRect();
    }

    void GraphicsDevice::updateClientRect() {
        auto size = this->size();
        _clientRect.first = size.first;
        _clientRect.second = size.second;
    }

    void GraphicsDevice::removeTarget(RenderTarget* target)
    {
        _targets.erase(target);
    }

    std::shared_ptr<VertexBuffer> GraphicsDevice::quadVertexBuffer()
    {
        if (_quadVertexBuffer) {
            return _quadVertexBuffer;
        }

        // DEVIATION: Metal/WebGPU texture UV origin is top-left (V=0 at top).
        // Upstream handles this via getImageEffectUV() Y-flip in shader.
        // We flip UV.y here in the vertex data so all post-processing fragment
        // shaders receive Metal-convention UVs matching texture layout.
        static constexpr QuadVertex quadVertices[4] = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
            {{ 1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
            {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
            {{ 1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}}
        };

        std::vector<uint8_t> data(sizeof(quadVertices));
        std::memcpy(data.data(), quadVertices, sizeof(quadVertices));

        auto format = std::make_shared<VertexFormat>(14 * static_cast<int>(sizeof(float)), true, false);
        VertexBufferOptions options;
        options.usage = BUFFER_STATIC;
        options.data = std::move(data);

        _quadVertexBuffer = createVertexBuffer(format, 4, options);
        return _quadVertexBuffer;
    }
}
