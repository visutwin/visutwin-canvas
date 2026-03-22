// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
// A script that creates a shadow-catching ground plane beneath the target entity.
// The plane uses a multiplicative-blend material with the LIT_SHADOW_CATCHER shader
// path, which outputs the accumulated shadow factor as a grayscale value. When blended
// multiplicatively with the framebuffer, lit areas remain unchanged while shadowed
// areas are darkened — giving the appearance of a shadow on the existing background.
//
#pragma once

#include <memory>

#include "script.h"
#include "scriptRegistry.h"

namespace visutwin::canvas
{
    class Entity;
    class StandardMaterial;

    /**
     * Shadow catcher script — creates an invisible ground plane that only renders shadows.
     *
     * Attach this script to an entity (typically the same entity or parent that holds the model).
     * The script creates a child plane entity with a special material that:
     *   - Uses multiplicative blending (BLEND_MULTIPLICATIVE)
     *   - Has the shadowCatcher flag set (triggers VT_FEATURE_SHADOW_CATCHER shader path)
     *   - Does not cast shadows itself
     *   - Has no diffuse/specular/emissive contribution
     *
     * mirrors the behavior of 
     */
    class ShadowCatcher : public Script
    {
    public:
        SCRIPT_NAME("shadowCatcher")

        // --- Configuration (can be set before or after initialize) ---

        // Scale of the shadow-catching plane (default 20x20)
        float planeScale() const { return _planeScale; }
        void setPlaneScale(float value);

        // Vertical offset below the entity position (default 0)
        float yOffset() const { return _yOffset; }
        void setYOffset(float value);

        // Shadow intensity: 0 = no shadow, 1 = full shadow darkening (default 1.0)
        float intensity() const { return _intensity; }
        void setIntensity(float value) { _intensity = value; }

        void initialize() override;

    private:
        float _planeScale = 20.0f;
        float _yOffset = 0.0f;
        float _intensity = 1.0f;

        Entity* _planeEntity = nullptr;
        StandardMaterial* _material = nullptr;
    };
}

REGISTER_SCRIPT(visutwin::canvas::ShadowCatcher, "shadowCatcher")
