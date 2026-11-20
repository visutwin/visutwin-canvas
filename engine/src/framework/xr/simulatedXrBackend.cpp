// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#include "simulatedXrBackend.h"

#include <algorithm>
#include <string>

#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    bool SimulatedXrBackend::isAvailable(const XrSessionType type)
    {
        return type == XrSessionType::Vr ? _options.vrAvailable : _options.arAvailable;
    }

    bool SimulatedXrBackend::begin(const XrSessionOptions& options, GraphicsDevice& device, std::string& error)
    {
        const int views = std::clamp(_options.viewCount, 1, 2);
        _width = std::max(1, static_cast<int>(static_cast<float>(_options.viewWidth) * options.framebufferScaleFactor));
        _height = std::max(1, static_cast<int>(static_cast<float>(_options.viewHeight) * options.framebufferScaleFactor));
        if (_width > device.maxTextureSize() || _height > device.maxTextureSize()) {
            error = "the simulated views are larger than the device's textures";
            return false;
        }
        _slices = std::max(options.depthSlices, 1);

        _images.clear();
        for (int view = 0; view < views; ++view) {
            for (int slice = 0; slice < _slices; ++slice) {
                const std::string name = "XrView" + std::to_string(view) + "Slice" + std::to_string(slice);
                TextureOptions textureOptions;
                textureOptions.name = name;
                textureOptions.width = _width;
                textureOptions.height = _height;
                textureOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
                textureOptions.mipmaps = false;
                textureOptions.minFilter = FilterMode::FILTER_LINEAR;
                textureOptions.magFilter = FilterMode::FILTER_LINEAR;
                Image image;
                image.color = std::make_shared<Texture>(&device, textureOptions);
                image.color->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
                image.color->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

                RenderTargetOptions targetOptions;
                targetOptions.graphicsDevice = &device;
                targetOptions.colorBuffer = image.color.get();
                targetOptions.depth = true;
                targetOptions.samples = std::min(options.samples, device.maxSamples());
                targetOptions.name = name;
                image.target = device.createRenderTarget(targetOptions);
                if (!image.target) {
                    _images.clear();
                    error = "the device could not create a target for a simulated view";
                    return false;
                }
                _images.push_back(std::move(image));
            }
        }
        _running = true;
        _framesBegun = 0;
        _framesEnded = 0;
        return true;
    }

    void SimulatedXrBackend::end()
    {
        _images.clear();
        _running = false;
    }

    bool SimulatedXrBackend::waitFrame(XrFrameTiming& timing)
    {
        // No display to wait for: the window's own pacing is the frame rate.
        _time += 1.0 / 90.0;
        timing.shouldRender = _visibility != XrVisibility::Hidden;
        timing.predictedDisplayTime = _time;
        timing.predictedDisplayPeriod = 1.0 / 90.0;
        return _running;
    }

    bool SimulatedXrBackend::locateViews(std::vector<XrViewState>& views)
    {
        if (!_tracking) {
            return false;
        }
        const int count = std::clamp(_options.viewCount, 1, 2);
        if (count == 1) {
            views.push_back({XrEye::None, _head, _options.fov});
            return true;
        }
        const float half = _options.interpupillaryDistance * 0.5f;
        const Vector3 right = _head.rotation * Vector3(1.0f, 0.0f, 0.0f);
        // The right eye takes the field of view as given; the left eye its mirror image.
        XrFov leftFov = _options.fov;
        leftFov.left = -_options.fov.right;
        leftFov.right = -_options.fov.left;
        views.push_back({XrEye::Left, {_head.position - right * half, _head.rotation}, leftFov});
        views.push_back({XrEye::Right, {_head.position + right * half, _head.rotation}, _options.fov});
        return true;
    }

    void SimulatedXrBackend::pollInput(std::vector<XrInputSourceState>& sources)
    {
        for (const auto& [id, state] : _sources) {
            sources.push_back(state);
        }
    }

    bool SimulatedXrBackend::beginFrame(std::vector<XrViewTarget>& targets)
    {
        ++_framesBegun;
        for (const auto& image : _images) {
            targets.push_back({image.target,
                Vector4(0.0f, 0.0f, static_cast<float>(_width), static_cast<float>(_height))});
        }
        return !_images.empty();
    }

    void SimulatedXrBackend::endFrame(const XrFrameSubmission& submission)
    {
        ++_framesEnded;
        _lastSubmission = submission;
    }

    Texture* SimulatedXrBackend::viewTexture(const int view, const int slice) const
    {
        const size_t index = static_cast<size_t>(view) * static_cast<size_t>(_slices) + static_cast<size_t>(slice);
        return view >= 0 && slice >= 0 && slice < _slices && index < _images.size() ? _images[index].color.get() : nullptr;
    }

    void SimulatedXrBackend::pulse(const uint32_t inputSourceId, const float intensity, float /*seconds*/)
    {
        _pulses.emplace_back(inputSourceId, intensity);
    }
}
