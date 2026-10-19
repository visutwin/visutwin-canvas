// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 09.02.2026
//
#pragma once

#include <memory>
#include <vector>

#include "platform/graphics/renderPass.h"
#include "scene/composition/renderAction.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Layer;
    class LayerComposition;
    class Scene;
    class Renderer;

    /**
     * A render pass rendering a set of layers with ONE camera. Every render action added
     * must use that camera: the pass clears for its first action only, so a second camera
     * sharing it would lose its clears, and a camera's directional shadows render before its
     * first pass, so a pass reaching into another camera's actions would draw them with the
     * wrong shadow fit.
     */
    class RenderPassForward final : public RenderPass
    {
    public:
        RenderPassForward(const std::shared_ptr<GraphicsDevice>& device,
            LayerComposition* layerComposition, Scene* scene, Renderer* renderer, CameraComponent* camera);

        /// Adds a render action of the pass's camera; one of another camera is refused.
        void addRenderAction(RenderAction* renderAction);

        /// Adds a layer the pass's camera renders, as a render action the pass owns: an
        /// application's own forward pass (a camera's render passes). With `autoClears` the
        /// first layer added takes the camera's and the layer's clear flags, a later one the
        /// layer's alone. Call after init(), which sets the target the actions render into.
        void addLayer(Layer* layer, bool transparent, bool autoClears = true);

        /// Marks the pass as the first / last rendering its camera in the frame, so it fires the
        /// scene's "prerender" / "postrender" for the camera: what the frame graph sets for a
        /// forward pass among a camera's render passes, whose actions no composition marks.
        void setCameraUse(const bool first, const bool last)
        {
            _firstCameraUse = first;
            _lastCameraUse = last;
        }
        [[nodiscard]] CameraComponent* camera() const { return _camera; }
        const std::vector<RenderAction*>& renderActions() const { return _renderActions; }

        // when true, the ProgramLibrary will generate forward shaders
        // that output linear HDR (no tonemapping, no gamma correction).
        void setHdrPass(bool hdr) { _hdrPass = hdr; }
        bool hdrPass() const { return _hdrPass; }

        void before() override;
        void execute() override;
        void after() override;
        void prepareShaders() override;

    private:
        void updateClears();
        bool validateRenderActionOrder() const;
        void renderRenderAction(RenderAction* renderAction, bool firstRenderAction);

        LayerComposition* _layerComposition = nullptr;
        Scene* _scene = nullptr;
        Renderer* _renderer = nullptr;
        CameraComponent* _camera = nullptr;
        std::vector<RenderAction*> _renderActions;
        std::vector<std::unique_ptr<RenderAction>> _ownedActions;
        bool _firstCameraUse = false;
        bool _lastCameraUse = false;

        bool _hdrPass = false;

        // Runtime parity checks for pass ordering.
        bool _beforeCalled = false;
        bool _executeCalled = false;
    };
}
