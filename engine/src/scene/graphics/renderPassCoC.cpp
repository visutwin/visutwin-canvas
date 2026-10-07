// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Circle-of-confusion pass for depth of field: writes (cocFar, cocNear) from
// linear scene depth. One implementation over QuadRender — a shader, one input
// texture and one uniform block — rather than a pass class per backend.
//
// Ramp: a dead zone of +/- focusRange/2 around the
// focus distance, then a ramp over the full focusRange, output as
// (cocFar, cocNear). The same ramp as applyDofSinglePass in the compose program (the
// compose fallback when no CoC texture is bound), so the two DOF paths agree.
//
// Part of the multi-pass DOF pipeline (CoC -> far downsample -> bokeh blur ->
// applyDof in compose) that RenderPassCameraFrame::setupDofPass() builds.
//
#include "renderPassCoC.h"

#include <algorithm>

#include "scene/camera.h"
#include "scene/graphics/quadRender.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    namespace
    {
        struct alignas(16) CoCUniforms
        {
            float focus[4];  // x=focusDistance, y=focusRange, z=cameraNear, w=cameraFar
            float flags[4];  // x=nearBlur
        };
        static_assert(sizeof(CoCUniforms) == 32);

    }

    RenderPassCoC::RenderPassCoC(const std::shared_ptr<GraphicsDevice>& device, CameraComponent* cameraComponent,
        const bool nearBlur)
        : RenderPassShaderQuad(device), _cameraComponent(cameraComponent), _nearBlur(nearBlur)
    {
    }

    void RenderPassCoC::prepareShaders()
    {
        if (!shader()) {
            useSlangShader("dof-coc");
        }
    }

    void RenderPassCoC::execute()
    {
        _params[0] = _focusDistance + 0.001f;
        _params[1] = std::max(_focusRange, 0.001f);
        _params[2] = 1.0f / _params[1];

        const auto* camera = _cameraComponent ? _cameraComponent->camera() : nullptr;
        if (!camera) return;

        const auto gd = device();
        if (!gd) return;

        Texture* depthTexture = gd->sceneDepthMap();
        if (!depthTexture) {
            return;
        }

        // Normally there already: the frame graph prepares every pass's shaders first.
        prepareShaders();
        if (!shader()) {
            return;
        }

        CoCUniforms uniforms{};
        uniforms.focus[0] = _focusDistance;
        uniforms.focus[1] = std::max(_focusRange, 0.001f);
        uniforms.focus[2] = camera->nearClip();
        uniforms.focus[3] = camera->farClip();
        uniforms.flags[0] = _nearBlur ? 1.0f : 0.0f;

        setQuadTextureBinding(0, depthTexture);
        setQuadUniforms(uniforms);
        RenderPassShaderQuad::execute();
    }
}
