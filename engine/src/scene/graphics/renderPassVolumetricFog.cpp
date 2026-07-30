// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
#include "renderPassVolumetricFog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "core/math/color.h"
#include "framework/components/light/lightComponent.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/camera.h"
#include "scene/graphNode.h"
#include "scene/light.h"
#include "scene/renderer/shadowMap.h"
#include "scene/scene.h"
#include "spdlog/spdlog.h"

namespace visutwin::canvas
{
    namespace
    {
        // The directional light the fog scatters. Upstream picks the first enabled, casting
        // directional light; mirror that.
        LightComponent* findDirectionalLight()
        {
            for (auto* component : LightComponent::instances()) {
                if (!component || !component->enabled() || !component->entity()) {
                    continue;
                }
                if (component->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                    return component;
                }
            }
            return nullptr;
        }
    }

    RenderPassVolumetricFog::RenderPassVolumetricFog(const std::shared_ptr<GraphicsDevice>& device,
        Texture* sourceTexture, CameraComponent* cameraComponent)
        : RenderPassShaderQuad(device), _sourceTexture(sourceTexture), _cameraComponent(cameraComponent)
    {
        // rgb = in-scattered light (HDR), a = transmittance. Linear filtering so the combine pass
        // can sample between texels.
        TextureOptions textureOptions;
        textureOptions.name = "VolumetricFogTexture";
        textureOptions.width = 1;
        textureOptions.height = 1;
        textureOptions.format = PixelFormat::PIXELFORMAT_RGBA16F;
        textureOptions.mipmaps = false;
        textureOptions.minFilter = FilterMode::FILTER_LINEAR;
        textureOptions.magFilter = FilterMode::FILTER_LINEAR;
        _fogTexture = std::make_shared<Texture>(device.get(), textureOptions);
        _fogTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        _fogTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        RenderTargetOptions rtOptions;
        rtOptions.graphicsDevice = device.get();
        rtOptions.colorBuffer = _fogTexture.get();
        rtOptions.depth = false;
        rtOptions.stencil = false;
        rtOptions.name = "VolumetricFogTarget";
        _fogRenderTarget = device->createRenderTarget(rtOptions);

        setScale(_scale);

        // The march writes every pixel, so the clear only matters before the first draw.
        const Color clearBlack(0.0f, 0.0f, 0.0f, 1.0f);
        setClearColor(&clearBlack);
    }

    RenderPassVolumetricFog::~RenderPassVolumetricFog() = default;

    void RenderPassVolumetricFog::setSettings(const VolumetricFogSettings& settings)
    {
        _settings = settings;
        if (_scale != settings.scale) {
            setScale(settings.scale);
        }
    }

    void RenderPassVolumetricFog::setScale(const float value)
    {
        _scale = std::clamp(value, 0.05f, 1.0f);
        auto options = std::make_shared<RenderPassOptions>();
        options->resizeSource = std::shared_ptr<Texture>(_sourceTexture, [](Texture*) {});
        options->scaleX = _scale;
        options->scaleY = _scale;
        setOptions(options);
        init(_fogRenderTarget, options);
    }

    void RenderPassVolumetricFog::execute()
    {
        const auto gd = device();
        if (!gd || !_cameraComponent || !_cameraComponent->camera()) {
            return;
        }

        Texture* depthTexture = gd->sceneDepthMap();
        if (!depthTexture) {
            return;
        }

        auto* camera = _cameraComponent->camera();
        auto* cameraNode = camera->node();
        if (!cameraNode) {
            return;
        }

        VolumetricFogPassParams params;
        params.depthTexture = depthTexture;

        // The camera's world transform IS the inverse view matrix.
        const Matrix4 cameraWorld = cameraNode->worldTransform();
        params.invView = cameraWorld;

        const Vector3 cameraPosition(cameraWorld.getColumn(3));
        params.cameraPosition[0] = cameraPosition.getX();
        params.cameraPosition[1] = cameraPosition.getY();
        params.cameraPosition[2] = cameraPosition.getZ();

        // The camera looks down its local -Z.
        Vector3 forward = Vector3(cameraWorld.getColumn(2)) * -1.0f;
        if (forward.lengthSquared() > 1e-8f) {
            forward = forward.normalized();
        } else {
            forward = Vector3(0.0f, 0.0f, -1.0f);
        }
        params.cameraForward[0] = forward.getX();
        params.cameraForward[1] = forward.getY();
        params.cameraForward[2] = forward.getZ();

        // NDC -> near plane scale. Mirrors the perspective half-size derivation.
        const float fovRad = camera->fov() * (std::numbers::pi_v<float> / 180.0f);
        const float aspect = camera->aspectRatio();
        if (camera->horizontalFov()) {
            params.projScaleX = std::tan(fovRad * 0.5f);
            params.projScaleY = params.projScaleX / std::max(aspect, 1e-4f);
        } else {
            params.projScaleY = std::tan(fovRad * 0.5f);
            params.projScaleX = params.projScaleY * aspect;
        }

        params.cameraNear = camera->nearClip();
        params.cameraFar = camera->farClip();

        params.density = std::max(_settings.density, 0.0f);
        params.heightBase = _settings.heightBase;
        params.heightFalloff = std::max(_settings.heightFalloff, 0.0f);
        params.maxDistance = std::max(_settings.maxDistance, 1e-3f);
        params.anisotropy = std::clamp(_settings.anisotropy, -0.95f, 0.95f);
        params.stepCount = static_cast<float>(std::max(_settings.steps, 1));
        params.shadowIntensity = std::clamp(_settings.shadowIntensity, 0.0f, 1.0f);
        params.extinction = std::max(_settings.extinction, 0.0f);

        for (int i = 0; i < 3; ++i) {
            params.tint[i] = _settings.tint[i];
        }

        // Match the lit scene: the forward pass multiplies light by the scene exposure, so the
        // fog's in-scattering has to be scaled the same way or it will not sit in the same range.
        const float exposure = _scene ? _scene->exposure() : 1.0f;

        auto* lightComponent = findDirectionalLight();
        if (lightComponent) {
            const Color& color = lightComponent->color();
            const float scale = _settings.intensity * lightComponent->intensity() * exposure;
            params.lightColor[0] = color.r * scale;
            params.lightColor[1] = color.g * scale;
            params.lightColor[2] = color.b * scale;

            // direction() is the direction light travels; the phase function wants the direction
            // towards the light (same convention as the forward pass's `L = -lightDir`).
            Vector3 towardsLight = lightComponent->direction() * -1.0f;
            if (towardsLight.lengthSquared() > 1e-8f) {
                towardsLight = towardsLight.normalized();
            } else {
                towardsLight = Vector3(0.0f, 1.0f, 0.0f);
            }
            params.lightDirection[0] = towardsLight.getX();
            params.lightDirection[1] = towardsLight.getY();
            params.lightDirection[2] = towardsLight.getZ();

            // Shadow cascades, when the light actually rendered a shadow map this frame.
            Light* sceneLight = lightComponent->light();
            if (_settings.shadowIntensity > 0.0f && sceneLight && sceneLight->shadowMap() &&
                lightComponent->castShadows()) {
                params.shadowTexture = sceneLight->shadowMap()->shadowTexture();

                // The palette holds 4 column-major matrices back to back, each already baking in
                // projection, view, the atlas viewport remap and the [0,1] depth mapping.
                const auto& palette = sceneLight->shadowMatrixPalette();
                for (int cascade = 0; cascade < 4; ++cascade) {
                    // The palette is column-major, 16 floats per cascade, so element
                    // [col * 4 + row] maps straight onto setElement(col, row).
                    Matrix4& dest = params.shadowMatrixPalette[cascade];
                    for (int col = 0; col < 4; ++col) {
                        for (int row = 0; row < 4; ++row) {
                            dest.setElement(col, row, palette[cascade * 16 + col * 4 + row]);
                        }
                    }
                }
                const auto& distances = sceneLight->shadowCascadeDistances();
                for (int i = 0; i < 4; ++i) {
                    params.shadowCascadeDistances[i] = distances[i];
                }
                params.shadowCascadeCount = static_cast<float>(std::max(sceneLight->numCascades(), 1));
                // NOT sceneLight->shadowBias(): that value (typically 0.05) is the world-space
                // normal-bias scale. The depth comparison against the [0,1] cascade depth uses a
                // much smaller constant - renderer.cpp uses 0.0001 for the forward PCF path, and
                // the fog march has to match or every sample tests as lit.
                params.shadowBias = 0.0001f;
                params.shadowDistance = sceneLight->shadowDistance();
            }
        }

        for (int i = 0; i < 3; ++i) {
            params.ambient[i] = _settings.ambientColor[i] * _settings.ambientIntensity * exposure;
        }

        // Cycle the dither so a TAA-enabled camera resolves it to smooth fog.
        params.noiseOffset = static_cast<float>(_frameIndex % 8) * (1.0f / 8.0f);
        ++_frameIndex;

        gd->executeVolumetricFogPass(params);
    }

    // -----------------------------------------------------------------------------------------

    RenderPassVolumetricFogCombine::RenderPassVolumetricFogCombine(
        const std::shared_ptr<GraphicsDevice>& device, CameraComponent* cameraComponent,
        Texture* fogTexture)
        : RenderPassShaderQuad(device), _cameraComponent(cameraComponent), _fogTexture(fogTexture)
    {
    }

    void RenderPassVolumetricFogCombine::execute()
    {
        const auto gd = device();
        if (!gd || !_fogTexture || !_cameraComponent || !_cameraComponent->camera()) {
            return;
        }

        Texture* depthTexture = gd->sceneDepthMap();
        if (!depthTexture) {
            return;
        }

        const auto* camera = _cameraComponent->camera();

        VolumetricFogCombineParams params;
        params.depthTexture = depthTexture;
        params.fogTexture = _fogTexture;
        params.fogTextureWidth = static_cast<float>(std::max<uint32_t>(_fogTexture->width(), 1u));
        params.fogTextureHeight = static_cast<float>(std::max<uint32_t>(_fogTexture->height(), 1u));
        params.cameraNear = camera->nearClip();
        params.cameraFar = camera->farClip();

        gd->executeVolumetricFogCombinePass(params);
    }
}
