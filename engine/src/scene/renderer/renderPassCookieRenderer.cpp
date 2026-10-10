// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2025
//
#include "renderPassCookieRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "cookieBlitShaders.h"
#include "lightCamera.h"
#include "scene/shader-lib/slangShaders.h"
#include "platform/graphics/blendState.h"
#include "platform/graphics/depthState.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/shader.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/graphNode.h"
#include "scene/graphics/quadRender.h"
#include "scene/lighting/lightTextureAtlas.h"

namespace visutwin::canvas
{
    RenderPassCookieRenderer::RenderPassCookieRenderer(const std::shared_ptr<GraphicsDevice>& device,
        LightTextureAtlas* atlas)
        : RenderPass(device), _atlas(atlas)
    {
        _name = "CookieRenderer";
    }

    RenderPassCookieRenderer::~RenderPassCookieRenderer() = default;

    void RenderPassCookieRenderer::update(const std::vector<Light*>& lights)
    {
        if (!_atlas || !_atlas->cookieRenderTarget()) {
            _filteredLights.clear();
            return;
        }
        // The pass loads and stores the atlas: cookies already copied stay.
        if (renderTarget() != _atlas->cookieRenderTarget()) {
            init(_atlas->cookieRenderTarget());
            for (const auto& ops : colorArrayOps()) {
                ops->clear = false;
                ops->store = true;
            }
        }
        // A new or resized atlas has lost what it held: copy every cookie again.
        if (_atlasVersion != _atlas->cookieAtlasVersion()) {
            _atlasVersion = _atlas->cookieAtlasVersion();
            _forceCopy = true;
        }

        _filteredLights.clear();
        filter(lights, _filteredLights);
    }

    void RenderPassCookieRenderer::filter(const std::vector<Light*>& lights, std::vector<Light*>& filteredLights)
    {
        for (auto* light : lights) {
            // Skip directional lights
            if (!light || light->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                continue;
            }

            // Skip clustered cookies with no assigned atlas slot
            if (!light->atlasViewportAllocated()) {
                continue;
            }

            // Copy the cookie when the light's slot is reassigned or the cookie's content
            // changed since the last copy (a procedural or video cookie updates every frame).
            const bool cookieUpdated = light->cookie() &&
                light->cookie()->uploadVersion() != light->cookieRenderVersion();
            if (!light->atlasSlotUpdated() && !cookieUpdated && !_forceCopy) {
                continue;
            }

            if (light->enabled() && light->cookie() && light->visibleThisFrame()) {
                filteredLights.push_back(light);
            }
        }

        _forceCopy = false;
    }

    std::shared_ptr<Shader> RenderPassCookieRenderer::blitShader(const bool cube)
    {
        auto& shader = cube ? _shaderCube : _shader2D;
        if (shader) {
            return shader;
        }
        GraphicsDevice* device = this->device().get();
        shader = getOrCreateSlangShader(device, "cookie-blit", cube ? "cube" : "2d");
        return shader;
    }

    void RenderPassCookieRenderer::execute()
    {
        if (_filteredLights.empty() || !_atlas || !_atlas->cookieAtlasTexture()) {
            return;
        }
        GraphicsDevice* device = this->device().get();
        device->setBlendState(BlendState::noBlend());
        device->setDepthState(DepthState::noDepth());
        device->setCullMode(CullMode::CULLFACE_NONE);
        device->setStencilState();

        const float cookieSize = static_cast<float>(_atlas->cookieAtlasTexture()->width());
        const int shadowResolution = _atlas->resolution();

        for (Light* light : _filteredLights) {
            Texture* cookie = light->cookie();
            const Vector4 slot = light->atlasViewport();
            light->setCookieRenderVersion(cookie->uploadVersion());
            if (light->type() == LightType::LIGHTTYPE_OMNI) {
                if (!cookie->isCubemap()) {
                    continue;
                }
                auto shader = blitShader(true);
                if (!shader) {
                    continue;
                }
                QuadRender quad(shader);
                quad.setTexture(0, cookie);
                // The face cameras the clustered omni shadow renders with: 90 degrees
                // widened by the edge pixels of a tile of this slot, so the shader's
                // inset face UV reads cookie and shadow at the same texel.
                const float tileSize = std::max(static_cast<float>(shadowResolution) * slot.getZ() / 3.0f, 1.0f);
                const float filterSize = (2.0f / tileSize) * static_cast<float>(LightTextureAtlas::kShadowEdgePixels);
                const float fov = std::atan(1.0f + filterSize) * (180.0f / std::numbers::pi_v<float>) * 2.0f;
                for (int face = 0; face < 6; ++face) {
                    auto& camera = _faceCameras[static_cast<size_t>(face)];
                    if (!camera) {
                        camera.reset(LightCamera::create("CookieFaceCamera", LightType::LIGHTTYPE_OMNI, face));
                    }
                    camera->setFov(fov);
                    camera->setNearClip(0.01f);
                    camera->setFarClip(10.0f);
                    const Matrix4 viewProjection = camera->projectionMatrix()
                        * camera->node()->worldTransform().inverse();
                    cookie_shaders::CookieBlitUniforms uniforms;
                    viewProjection.inverse().store(uniforms.invViewProj);
                    quad.setUniforms(uniforms);
                    const Vector4 viewport = LightTextureAtlas::omniFaceRect(slot, face) * cookieSize;
                    quad.render(&viewport);
                }
            } else {
                if (cookie->isCubemap()) {
                    continue;
                }
                auto shader = blitShader(false);
                if (!shader) {
                    continue;
                }
                QuadRender quad(shader);
                quad.setTexture(0, cookie);
                quad.setUniforms(cookie_shaders::CookieBlitUniforms{});
                // The rect the spot's projection maps to (its shadow viewport).
                const Vector4 viewport = LightTextureAtlas::spotViewport(slot, shadowResolution,
                    LightTextureAtlas::kSpotEdgePixels) * cookieSize;
                quad.render(&viewport);
            }
        }
        _filteredLights.clear();
    }
}
