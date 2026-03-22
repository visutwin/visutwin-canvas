// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "renderPassPrepass.h"

#include "platform/graphics/graphicsDevice.h"

namespace visutwin::canvas
{
    RenderPassPrepass::RenderPassPrepass(const std::shared_ptr<GraphicsDevice>& device, Scene* scene, Renderer* renderer,
        CameraComponent* cameraComponent, Texture* sceneDepthTexture, const std::shared_ptr<RenderPassOptions>& options)
        : RenderPass(device), _scene(scene), _renderer(renderer), _cameraComponent(cameraComponent),
          _sceneDepthTexture(sceneDepthTexture)
    {
        // DEVIATION: renderer prepass shader pass is not fully ported; keep a lightweight pass node for parity.
        (void)_scene;
        (void)_renderer;
        (void)_cameraComponent;
        setOptions(options);
    }

    void RenderPassPrepass::execute()
    {
        // DEVIATION: full SHADER_PREPASS mesh submission path is pending renderer shader-pass parity.
    }

    void RenderPassPrepass::after()
    {
        RenderPass::after();
        if (const auto gd = device(); gd && _sceneDepthTexture) {
            gd->setSceneDepthMap(_sceneDepthTexture);
        }
    }
}

