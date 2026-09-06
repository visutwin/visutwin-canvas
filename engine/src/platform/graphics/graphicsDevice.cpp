// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025.
//
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "graphicsDevice.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    bool GraphicsDevice::supportsCompressedFormat(const PixelFormat format) const
    {
        return !isCompressedPixelFormat(format);
    }

    PixelFormat GraphicsDevice::preferredCompressedRgbaFormat() const
    {
        for (const PixelFormat candidate : {PixelFormat::PIXELFORMAT_ASTC_4x4,
                                            PixelFormat::PIXELFORMAT_BC7,
                                            PixelFormat::PIXELFORMAT_DXT5}) {
            if (supportsCompressedFormat(candidate)) {
                return candidate;
            }
        }
        return PixelFormat::PIXELFORMAT_RGBA8;
    }

    void logUnsupportedDeviceFeature(const char* name)
    {
        spdlog::warn("GraphicsDevice::{} is not implemented by the active backend — "
            "the feature it drives will be missing from the rendered output", name);
    }

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

    void GraphicsDevice::releaseGpuReferences()
    {
        _shader.reset();
        _shaderCache.clear();
        _vertexBuffers.clear();
        _quadVertexBuffer.reset();
        _renderTarget.reset();
        _backBuffer.reset();
        _textures.clear();
        _dynamicBuffers.reset();
        _gpuProfiler.reset();
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

        // Env-var driven capture, so any example can be screenshotted without
        // being modified. Resolved once; the request is raised on the target
        // frame and cleared by the backend once written.
        if (!_screenshotEnvChecked) {
            _screenshotEnvChecked = true;
            if (const char* path = std::getenv("VISUTWIN_SCREENSHOT"); path && *path) {
                _screenshotEnvPath = path;
                if (const char* frame = std::getenv("VISUTWIN_SCREENSHOT_FRAME"); frame && *frame) {
                    _screenshotEnvFrame = std::strtoull(frame, nullptr, 10);
                }
                spdlog::info("Screenshot armed: '{}' at frame {}", _screenshotEnvPath,
                    _screenshotEnvFrame);
            }
        }
        ++_frameCounter;
        if (!_screenshotEnvPath.empty() && _frameCounter >= _screenshotEnvFrame) {
            requestScreenshot(_screenshotEnvPath);
            _screenshotEnvPath.clear();
        }

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
        //
        // An oversized fullscreen TRIANGLE, not a quad: covering the screen with
        // one triangle avoids the diagonal seam where a two-triangle quad's
        // barycentric interpolation meets, and it is what every dedicated post
        // pass class used before those effects moved onto QuadRender. Keeping the
        // same geometry keeps their output bit-identical — with a quad, kernels
        // that key off small UV differences (the bilateral depth-aware blur most
        // of all) drifted in the low bits along depth discontinuities.
        static constexpr QuadVertex quadVertices[3] = {
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f,  1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f,  1.0f}},
            {{ 3.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {2.0f,  1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f,  1.0f}},
            {{-1.0f,  3.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, -1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, -1.0f}}
        };

        std::vector<uint8_t> data(sizeof(quadVertices));
        std::memcpy(data.data(), quadVertices, sizeof(quadVertices));

        auto format = std::make_shared<VertexFormat>(
            14 * static_cast<int>(sizeof(float)), VertexFormat::standardElements(), true, false);
        VertexBufferOptions options;
        options.usage = BUFFER_STATIC;
        options.data = std::move(data);

        _quadVertexBuffer = createVertexBuffer(format, 3, options);
        return _quadVertexBuffer;
    }
}
