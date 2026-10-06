// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.09.2025
//
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <cstring>

#include "graphicsDevice.h"

#include "computeInstanceCuller.h"
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
        // A backend detaches its resources at the top of its own destructor, where it can
        // still free their GPU objects; this catches a device that did not (a test stub).
        detachResources();

        // Clean up resources
        if (_quadVertexBuffer) {
            _quadVertexBuffer.reset();
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
        _gpuProfiler.reset();
        detachResources();
    }

    GraphicsDevice::LiveResourceCounts GraphicsDevice::liveResourceCounts()
    {
        LiveResourceCounts counts;
        std::lock_guard lock(_liveResourcesMutex);
        counts.textures = static_cast<int>(_liveTextures.size());
        counts.renderTargets = static_cast<int>(_liveRenderTargets.size());
        counts.indexBuffers = static_cast<int>(_liveIndexBuffers.size());
        for (const VertexBuffer* buffer : _liveVertexBuffers) {
            ++(buffer->storageUse() ? counts.storageBuffers : counts.vertexBuffers);
        }
        counts.shaders = _liveShaders->load(std::memory_order_relaxed);
        addBackendResourceCounts(counts);
        return counts;
    }

    void GraphicsDevice::detachResources()
    {
        // Taken out of the registries first: a resource's destructor (which detaching does
        // not run) and these loops must not both walk them. Targets go before textures,
        // because a target's attachments are textures.
        std::unordered_set<RenderTarget*> targets;
        std::unordered_set<VertexBuffer*> vertexBuffers;
        std::unordered_set<IndexBuffer*> indexBuffers;
        std::unordered_set<Texture*> textures;
        {
            std::lock_guard lock(_liveResourcesMutex);
            targets.swap(_liveRenderTargets);
            vertexBuffers.swap(_liveVertexBuffers);
            indexBuffers.swap(_liveIndexBuffers);
            textures.swap(_liveTextures);
        }
        for (RenderTarget* target : targets) {
            target->detachFromDevice();
        }
        for (VertexBuffer* buffer : vertexBuffers) {
            buffer->detachFromDevice();
        }
        for (IndexBuffer* buffer : indexBuffers) {
            buffer->detachFromDevice();
        }
        for (Texture* texture : textures) {
            texture->detachFromDevice();
        }
    }

    uint64_t GraphicsDevice::nextPaletteVersion()
    {
        static uint64_t version = 0;
        return ++version;
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
                if (const char* count = std::getenv("VISUTWIN_SCREENSHOT_COUNT"); count && *count) {
                    _screenshotEnvCount = std::max<uint64_t>(std::strtoull(count, nullptr, 10), 1);
                }
                if (const char* time = std::getenv("VISUTWIN_SCREENSHOT_TIME"); time && *time) {
                    _screenshotEnvTime = std::strtod(time, nullptr);
                }
                spdlog::info("Screenshot armed: '{}' at frame {} x{}{}", _screenshotEnvPath,
                    _screenshotEnvFrame, _screenshotEnvCount,
                    _screenshotEnvTime >= 0.0 ? " (by time: " + std::to_string(_screenshotEnvTime) + " s)" : "");
            }
        }
        ++_frameCounter;
        if (!_firstFrameSeen) {
            _firstFrameSeen = true;
            _firstFrameTime = std::chrono::steady_clock::now();
        }
        const bool armedNow = _screenshotEnvTime >= 0.0
            ? std::chrono::duration<double>(std::chrono::steady_clock::now() - _firstFrameTime).count() >= _screenshotEnvTime
            : _frameCounter >= _screenshotEnvFrame;
        if (!_screenshotEnvPath.empty() && armedNow) {
            if (_screenshotEnvCount > 1) {
                // A burst: number each frame so consecutive frames of one run can be diffed.
                const size_t dot = _screenshotEnvPath.rfind('.');
                const std::string stem = dot == std::string::npos ? _screenshotEnvPath : _screenshotEnvPath.substr(0, dot);
                const std::string ext = dot == std::string::npos ? std::string() : _screenshotEnvPath.substr(dot);
                requestScreenshot(stem + "_" + std::to_string(_frameCounter) + ext);
            } else {
                requestScreenshot(_screenshotEnvPath);
            }
            if (--_screenshotEnvCount == 0) {
                _screenshotEnvPath.clear();
            }
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

    void GraphicsDevice::resizeCanvas(const int width, const int height)
    {
        const float ratio = pixelRatio();
        const int w = static_cast<int>(std::floor(static_cast<float>(width) * ratio));
        const int h = static_cast<int>(std::floor(static_cast<float>(height) * ratio));
        const auto size = this->size();
        if (w != size.first || h != size.second) {
            setResolution(w, h);
        }
    }

    void GraphicsDevice::setMaxPixelRatio(const float value)
    {
        if (!(value > 0.0f)) {
            return;
        }
        _maxPixelRatio = value;
        const auto [w, h] = windowSizeInPoints();
        if (w > 0 && h > 0) {
            resizeCanvas(w, h);
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

    std::shared_ptr<VertexBuffer> GraphicsDevice::quadVertexBuffer()
    {
        if (_quadVertexBuffer) {
            return _quadVertexBuffer;
        }

        // DEVIATION: Metal/WebGPU texture UV origin is top-left (V=0 at top).
        // Upstream flips Y in the shader instead.
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

    std::unique_ptr<InstanceCuller> GraphicsDevice::createInstanceCuller()
    {
        if (!supportsCompute()) {
            return nullptr;
        }
        return std::make_unique<ComputeInstanceCuller>(this);
    }
}
