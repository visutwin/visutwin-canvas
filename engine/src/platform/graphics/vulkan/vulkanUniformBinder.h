// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Vulkan uniform binder — descriptor sets + push constants.
//
#pragma once

#ifdef VISUTWIN_HAS_VULKAN

#include <vulkan/vulkan.h>

#include "platform/graphics/uniformBinder.h"

namespace visutwin::canvas
{
    class VulkanUniformBinder : public UniformBinder
    {
    public:
        VulkanUniformBinder() = default;
        ~VulkanUniformBinder() override = default;

        void resetPassState() override;

        [[nodiscard]] bool isMaterialChanged(const Material* mat) const override;

        [[nodiscard]] Texture* envAtlasTexture() const override { return _envAtlasTexture; }
        [[nodiscard]] Texture* skyboxCubeMapTexture() const override { return _skyboxCubeMapTexture; }
        [[nodiscard]] Texture* shadowTexture() const override { return _shadowTexture; }
        [[nodiscard]] Texture* localShadowTexture0() const override { return _localShadowTexture0; }
        [[nodiscard]] Texture* localShadowTexture1() const override { return _localShadowTexture1; }
        [[nodiscard]] Texture* omniShadowCube0() const override { return _omniShadowCube0; }
        [[nodiscard]] Texture* omniShadowCube1() const override { return _omniShadowCube1; }

    private:
        Texture* _envAtlasTexture = nullptr;
        Texture* _skyboxCubeMapTexture = nullptr;
        Texture* _shadowTexture = nullptr;
        Texture* _localShadowTexture0 = nullptr;
        Texture* _localShadowTexture1 = nullptr;
        Texture* _omniShadowCube0 = nullptr;
        Texture* _omniShadowCube1 = nullptr;

        bool _materialBoundThisPass = false;
        const Material* _lastBoundMaterial = nullptr;
    };
}

#endif // VISUTWIN_HAS_VULKAN
