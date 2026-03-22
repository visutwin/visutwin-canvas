// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 11.12.2025.
//
#pragma once

#include "renderer.h"
#include "platform/graphics/renderPass.h"

namespace visutwin::canvas
{
    class RenderPassCompose;
    class RenderPassDof;
    class RenderPassSsao;
    class RenderPassTAA;

    /**
     * A render pass used to render post-effects.
     */
    class RenderPassPostprocessing : public RenderPass
    {
    public:
        RenderPassPostprocessing(const std::shared_ptr<GraphicsDevice>& device, Renderer* renderer, RenderAction* renderAction);

        void execute() override;

    private:
        Renderer* _renderer = nullptr;
        RenderAction* _renderAction = nullptr;
        std::shared_ptr<RenderPassDof> _dofPass;
        std::shared_ptr<RenderPassSsao> _ssaoPass;
        std::shared_ptr<RenderPassTAA> _taaPass;
        std::shared_ptr<RenderPassCompose> _composePass;
        bool _passesBuilt = false;
    };
}
