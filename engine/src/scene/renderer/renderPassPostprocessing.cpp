// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.12.2025.
//
#include "renderPassPostprocessing.h"

#include "scene/graphics/renderPassCompose.h"
#include "scene/graphics/renderPassDof.h"
#include "scene/graphics/renderPassSsao.h"
#include "scene/graphics/renderPassTAA.h"

namespace visutwin::canvas
{
    RenderPassPostprocessing::RenderPassPostprocessing(const std::shared_ptr<GraphicsDevice>& device, Renderer* renderer,
        RenderAction* renderAction): RenderPass(device), _renderer(renderer), _renderAction(renderAction)
    {
        init(nullptr);

        if (!_renderAction || !_renderAction->camera) {
            return;
        }

        const auto& dof = _renderAction->camera->dof();
        const auto& taa = _renderAction->camera->taa();
        _composePass = std::make_shared<RenderPassCompose>(device);

        auto* camera = _renderAction->camera->camera();
        Texture* sceneTexture = camera && camera->renderTarget()
            ? camera->renderTarget()->colorBuffer()
            : nullptr;
        if (!sceneTexture) {
            const auto dofRt = _renderAction->camera->dofSceneRenderTarget();
            sceneTexture = dofRt ? dofRt->colorBuffer() : nullptr;
        }

        if (taa.enabled && sceneTexture) {
            _taaPass = _renderAction->camera->ensureTaaPass(device, sceneTexture);
            if (_taaPass) {
                _taaPass->setSourceTexture(sceneTexture);
                sceneTexture = _taaPass->update().get();
                addBeforePass(_taaPass);
            }
        }

        // SSAO pass (runs before compose, reads depth buffer)
        const auto& ssao = _renderAction->camera->ssao();
        if (ssao.enabled && sceneTexture) {
            _ssaoPass = std::make_shared<RenderPassSsao>(
                device, sceneTexture, _renderAction->camera, ssao.blurEnabled);
            _ssaoPass->radius = ssao.radius;
            _ssaoPass->intensity = ssao.intensity;
            _ssaoPass->power = ssao.power;
            _ssaoPass->sampleCount = ssao.samples;
            _ssaoPass->minAngle = ssao.minAngle;
            _ssaoPass->randomize = ssao.randomize;
            if (ssao.scale != 1.0f) {
                _ssaoPass->setScale(ssao.scale);
            }
            addBeforePass(_ssaoPass);
        }

        _composePass->sceneTexture = sceneTexture;
        _composePass->ssaoTexture = _ssaoPass ? _ssaoPass->ssaoTexture() : nullptr;
        _composePass->dofEnabled = dof.enabled;

        if (dof.enabled && sceneTexture) {
            _dofPass = std::make_shared<RenderPassDof>(device, _renderAction->camera, sceneTexture, sceneTexture,
                dof.highQuality, dof.nearBlur);
            _dofPass->focusDistance = dof.focusDistance;
            _dofPass->focusRange = dof.focusRange;
            _dofPass->blurRadius = dof.blurRadius;
            _dofPass->blurRings = dof.blurRings;
            _dofPass->blurRingPoints = dof.blurRingPoints;
            _dofPass->highQuality = dof.highQuality;
            _dofPass->nearBlur = dof.nearBlur;
            _composePass->cocTexture = _dofPass->cocTexture();
            _composePass->blurTexture = _dofPass->blurTexture();
            _composePass->dofEnabled = true;
            addBeforePass(_dofPass);
            _passesBuilt = true;
        }

        if (_composePass) {
            _composePass->dofIntensity = 1.0f;
        }
    }

    void RenderPassPostprocessing::execute()
    {
        if (!_composePass) {
            return;
        }

        _composePass->execute();
    }
}
