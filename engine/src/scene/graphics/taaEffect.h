// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 15.10.2026
//
// The temporal anti-aliasing as a camera frame effect at the RESOLVE stage: the camera is
// offset by a different fraction of a pixel every frame (cameraJitter), and each frame is
// accumulated into a history reprojected through the scene depth (RenderPassTAA). The
// history replaces the scene colour for everything after it, so the half-resolution copy,
// depth of field, bloom and the composition all read the anti-aliased scene.
//
// DEVIATION: a built-in, owned by its camera (CameraComponent::taaEffect) and configured by
// CameraComponent::taa(), not registered in the camera's effect list. Its requirements are
// the frame's own (CameraFrameOptions::taaEnabled), so it is not part of the user effects'
// rebuild key. The pass and its history survive a rebuild of the frame's passes while TAA
// stays enabled; disabling it releases them.
//
#pragma once

#include <memory>

#include "cameraFrameEffect.h"

namespace visutwin::canvas
{
    class RenderPassTAA;

    class TaaEffect final : public CameraFrameEffect
    {
    public:
        explicit TaaEffect(CameraComponent* camera);
        ~TaaEffect() override;

        /// Enabled and the camera's TAA switched on.
        [[nodiscard]] bool active() const override;
        [[nodiscard]] bool ownsPasses() const override { return true; }
        [[nodiscard]] float cameraJitter() const override;

        void createPasses(const CameraFrameEffectResources& resources, CameraFrameEffectPasses& passes) override;
        void destroyPasses() override;
        void update() override;
        void resolveScene(CameraFrameEffectScene& scene) override;

        /// The exposure the composition displays the resolved scene with (1 under tone
        /// mapping NONE, which applies none): the history is blended at that exposure.
        void setDisplayExposure(float value);

        /// The pass, while the frame has one.
        [[nodiscard]] const std::shared_ptr<RenderPassTAA>& pass() const { return _pass; }

    private:
        CameraComponent* _camera = nullptr;
        std::shared_ptr<RenderPassTAA> _pass;
        float _displayExposure = 1.0f;
    };
}
