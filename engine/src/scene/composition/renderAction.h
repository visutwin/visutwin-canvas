// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 12.09.2025.
//
#pragma once

#include "framework/components/camera/cameraComponent.h"
#include "scene/layer.h"

namespace visutwin::canvas
{
    struct RenderAction
    {
        CameraComponent* camera = nullptr;

        // true if the camera should render using render passes, it specifies
        bool useCameraPasses = false;

        Layer* layer = nullptr;

        std::shared_ptr<RenderTarget> renderTarget;

        // True if this is the first render action using this camera
        bool firstCameraUse = false;

        // True if this render action should trigger a postprocessing callback for the camera
        bool triggerPostprocess = false;

        bool transparent = false;
        bool clearColor = false;
        bool clearDepth = false;
        bool clearStencil = false;

        // True if this is the last render action using this camera
        bool lastCameraUse = false;

        void setupClears(const CameraComponent* cameraComponent, const Layer* layer)
        {
            const auto* camera = cameraComponent ? cameraComponent->camera() : nullptr;
            clearColor = (camera && camera->clearColorBufferEnabled()) || (layer && layer->clearColorBuffer());
            clearDepth = (camera && camera->clearDepthBufferEnabled()) || (layer && layer->clearDepthBuffer());
            clearStencil = (camera && camera->clearStencilBufferEnabled()) || (layer && layer->clearStencilBuffer());
        }
    };
}
