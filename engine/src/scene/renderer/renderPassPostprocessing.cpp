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
            _ssaoPass->setRadius(ssao.radius);
            _ssaoPass->setIntensity(ssao.intensity);
            _ssaoPass->setPower(ssao.power);
            _ssaoPass->setSampleCount(ssao.samples);
            _ssaoPass->setMinAngle(ssao.minAngle);
            _ssaoPass->setRandomize(ssao.randomize);
            if (ssao.scale != 1.0f) {
                _ssaoPass->setScale(ssao.scale);
            }
            addBeforePass(_ssaoPass);
        }

        _composePass->setSceneTexture(sceneTexture);
        _composePass->setSsaoTexture(_ssaoPass ? _ssaoPass->ssaoTexture() : nullptr);
        _composePass->setDofEnabled(dof.enabled);

        if (dof.enabled && sceneTexture) {
            _dofPass = std::make_shared<RenderPassDof>(device, _renderAction->camera, sceneTexture, sceneTexture,
                dof.highQuality, dof.nearBlur);
            _dofPass->setFocusDistance(dof.focusDistance);
            _dofPass->setFocusRange(dof.focusRange);
            _dofPass->setBlurRadius(dof.blurRadius);
            _dofPass->setBlurRings(dof.blurRings);
            _dofPass->setBlurRingPoints(dof.blurRingPoints);
            _dofPass->setHighQuality(dof.highQuality);
            _dofPass->setNearBlur(dof.nearBlur);
            _composePass->setCocTexture(_dofPass->cocTexture());
            _composePass->setBlurTexture(_dofPass->blurTexture());
            _composePass->setDofEnabled(true);
            addBeforePass(_dofPass);
            _passesBuilt = true;
        }

        if (_composePass) {
            _composePass->setDofIntensity(1.0f);
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
