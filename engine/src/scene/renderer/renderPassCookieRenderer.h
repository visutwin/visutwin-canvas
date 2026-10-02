// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2025.
//
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "platform/graphics/renderPass.h"
#include "scene/light.h"

namespace visutwin::canvas
{
    class Camera;
    class LightTextureAtlas;
    class Shader;

    /**
     * Upstream RenderPassCookieRenderer: copies the cookie of every clustered spot and
     * omni light into its slot of the cookie atlas, so the cluster loop samples cookies
     * from one texture (see LightTextureAtlas). A spot's 2D cookie fills the rect its
     * projection maps to; an omni light's cube cookie is drawn face by face into the
     * same 3x2 tiles its shadow uses, each face through the camera that renders the
     * shadow face, so a direction lands on the same atlas texel in both.
     *
     * A cookie is copied only when its light was given a different slot, or the atlas
     * lost its contents (the cookie texture is taken to be static, as upstream does
     * unless the texture's upload version changes, which this port does not track).
     *
     * DEVIATION, in the exact direction: upstream copies a spot cookie into the WHOLE
     * slot and an omni face at exactly 90 degrees, then samples them through the
     * shadow's inset rect and widened faces, a few pixels of stretch; here the copy
     * targets the rects the sampling uses.
     */
    class RenderPassCookieRenderer : public RenderPass
    {
    public:
        RenderPassCookieRenderer(const std::shared_ptr<GraphicsDevice>& device, LightTextureAtlas* atlas);
        ~RenderPassCookieRenderer() override;

        void update(const std::vector<Light*>& lights);

        void execute() override;

    private:
        void filter(const std::vector<Light*>& lights, std::vector<Light*>& filteredLights);
        std::shared_ptr<Shader> blitShader(bool cube);

        LightTextureAtlas* _atlas = nullptr;
        std::vector<Light*> _filteredLights;
        std::shared_ptr<Shader> _shader2D;
        std::shared_ptr<Shader> _shaderCube;
        std::array<std::unique_ptr<Camera>, 6> _faceCameras;
        int _atlasVersion = -1;
        bool _forceCopy = false;
    };
}
