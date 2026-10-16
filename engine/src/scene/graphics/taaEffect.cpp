// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.10.2026
//
#include "taaEffect.h"

#include <algorithm>

#include "framework/components/camera/cameraComponent.h"
#include "renderPassTAA.h"

namespace visutwin::canvas
{
    TaaEffect::TaaEffect(CameraComponent* camera)
        : CameraFrameEffect("taa", {FrameResource::Depth}), _camera(camera)
    {
    }

    TaaEffect::~TaaEffect() = default;

    bool TaaEffect::active() const
    {
        return enabled() && _camera && _camera->taa().enabled;
    }

    float TaaEffect::cameraJitter() const
    {
        return _camera ? std::max(_camera->taa().jitter, 0.0f) : 0.0f;
    }

    void TaaEffect::createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes)
    {
        // The history outlives the frame's rebuilds: a pass kept from before takes the new
        // scene colour and depth, and accumulates on.
        const bool upscale = _camera && _camera->taa().upscale;
        if (!_pass || _pass->upscale() != upscale) {
            _pass = std::make_shared<RenderPassTAA>(resources.device, resources.sceneColor, _camera, upscale);
        } else {
            _pass->setSourceTexture(resources.sceneColor);
        }
        _pass->setDepthTexture(resources.depth);
        _pass->setDisplayExposure(_displayExposure);
        update();
        passes.resolve.push_back(_pass);
    }

    void TaaEffect::destroyPasses()
    {
        // Released only once TAA is off: a rebuild for anything else keeps the history.
        if (!active()) {
            _pass.reset();
        }
    }

    void TaaEffect::update()
    {
        if (_pass && _camera) {
            _pass->setHighQuality(_camera->taa().highQuality);
        }
    }

    void TaaEffect::resolveScene(CameraFrameEffectScene& scene)
    {
        if (!_pass) {
            return;
        }
        // The history is double buffered: the pass renders into the other texture each frame,
        // and that is the resolved scene. Before the first resolve has rendered there is no
        // history to read, so the frame reads the scene itself.
        _pass->setSourceTexture(scene.texture);
        // Upscaling, the history has the output's resolution, and so has the resolved scene.
        if (_pass->upscale() && scene.outputWidth > 0 && scene.outputHeight > 0) {
            _pass->setOutputSize(scene.outputWidth, scene.outputHeight);
        }
        const auto history = _pass->update();
        if (_pass->historyValid() && history) {
            scene.texture = history.get();
            if (_pass->upscale()) {
                scene.width = static_cast<int>(history->width());
                scene.height = static_cast<int>(history->height());
            }
        }
    }

    void TaaEffect::setDisplayExposure(const float value)
    {
        _displayExposure = value;
        if (_pass) {
            _pass->setDisplayExposure(value);
        }
    }
}
