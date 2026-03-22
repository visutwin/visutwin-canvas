// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026.
//
#pragma once

#include "platform/graphics/renderPass.h"
#include "scene/composition/renderAction.h"

namespace visutwin::canvas
{
    class LayerComposition;
    class Scene;
    class Renderer;

    /**
     * A render pass used to render a set of layers using a camera.
     *
     */
    class RenderPassForward final : public RenderPass
    {
    public:
        RenderPassForward(const std::shared_ptr<GraphicsDevice>& device,
            LayerComposition* layerComposition, Scene* scene, Renderer* renderer);

        void addRenderAction(RenderAction* renderAction);

        // when true, the ProgramLibrary will generate forward shaders
        // that output linear HDR (no tonemapping, no gamma correction).
        void setHdrPass(bool hdr) { _hdrPass = hdr; }
        bool hdrPass() const { return _hdrPass; }

        void before() override;
        void execute() override;
        void after() override;

    private:
        void updateClears();
        void refreshCameraUseFlags();
        bool validateRenderActionOrder() const;
        void renderRenderAction(RenderAction* renderAction, bool firstRenderAction);

        LayerComposition* _layerComposition = nullptr;
        Scene* _scene = nullptr;
        Renderer* _renderer = nullptr;
        std::vector<RenderAction*> _renderActions;

        bool _hdrPass = false;

        // Runtime parity checks for pass ordering.
        bool _beforeCalled = false;
        bool _executeCalled = false;
    };
}
