// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2025.
//
#include "shadowMap.h"

#include "scene/constants.h"
#include "scene/light.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/texture.h"

namespace visutwin::canvas
{
    std::unique_ptr<ShadowMap> ShadowMap::create(GraphicsDevice* device, Light* light)
    {
        if (!device || !light) {
            return nullptr;
        }

        const int resolution = light->shadowResolution();
        const ShadowType shadowType = light->shadowType();
        const auto& info = shadowTypeInfo.at(shadowType);

        auto shadowMap = std::make_unique<ShadowMap>();

        const bool isOmni = (light->type() == LightType::LIGHTTYPE_OMNI);

        // Create the depth texture for the shadow map.
        // Omni lights use a cubemap depth texture (6 faces), others use 2D.
        TextureOptions depthOptions;
        depthOptions.name = isOmni ? "OmniShadowCube" : "ShadowMap";
        depthOptions.width = static_cast<uint32_t>(resolution);
        depthOptions.height = static_cast<uint32_t>(resolution);
        depthOptions.format = info.format;
        depthOptions.mipmaps = false;
        depthOptions.minFilter = FilterMode::FILTER_NEAREST;
        depthOptions.magFilter = FilterMode::FILTER_NEAREST;
        depthOptions.cubemap = isOmni;
        shadowMap->_shadowTexture = std::make_shared<Texture>(device, depthOptions);
        shadowMap->_shadowTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
        shadowMap->_shadowTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

        // Create render target(s). Directional gets 1 (cascades use viewport sub-regions),
        // spot gets 1, omni gets 6 (cubemap faces).
        //create2dMap() returns [target] for directional.
        const int faceCount = (light->type() == LightType::LIGHTTYPE_DIRECTIONAL)
            ? 1 : light->numShadowFaces();
        for (int face = 0; face < faceCount; ++face) {
            RenderTargetOptions rtOptions;
            rtOptions.graphicsDevice = device;
            rtOptions.name = "ShadowMapRT-face" + std::to_string(face);
            rtOptions.face = face;  // Cubemap face index (used by Metal render pass for slice selection)

            if (info.pcf) {
                // PCF uses depth-only render target.
                rtOptions.depthBuffer = shadowMap->_shadowTexture.get();
                rtOptions.depth = true;
            } else {
                // VSM uses color render target (depth written to color channels).
                rtOptions.colorBuffer = shadowMap->_shadowTexture.get();
                rtOptions.depth = true;
            }

            shadowMap->_renderTargets.push_back(device->createRenderTarget(rtOptions));
        }

        // VSM ping-pong intermediate for the separable gaussian blur.
        // Same size + format as the main shadow map; no depth buffer needed
        // (blur is a full-screen quad with depth-test disabled).
        if (info.vsm) {
            TextureOptions tempOptions;
            tempOptions.name = "ShadowMapVsmBlurTemp";
            tempOptions.width = static_cast<uint32_t>(resolution);
            tempOptions.height = static_cast<uint32_t>(resolution);
            tempOptions.format = info.format;
            tempOptions.mipmaps = false;
            tempOptions.minFilter = FilterMode::FILTER_LINEAR;
            tempOptions.magFilter = FilterMode::FILTER_LINEAR;
            shadowMap->_blurTempTexture = std::make_shared<Texture>(device, tempOptions);
            shadowMap->_blurTempTexture->setAddressU(AddressMode::ADDRESS_CLAMP_TO_EDGE);
            shadowMap->_blurTempTexture->setAddressV(AddressMode::ADDRESS_CLAMP_TO_EDGE);

            RenderTargetOptions blurRtOptions;
            blurRtOptions.graphicsDevice = device;
            blurRtOptions.name = "ShadowMapVsmBlurTempRT";
            blurRtOptions.colorBuffer = shadowMap->_blurTempTexture.get();
            blurRtOptions.depth = false;
            shadowMap->_blurTempRenderTarget = device->createRenderTarget(blurRtOptions);
        }

        return shadowMap;
    }
}
