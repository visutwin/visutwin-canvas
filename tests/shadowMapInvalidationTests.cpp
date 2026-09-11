// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A Light's shadow map is allocated lazily by the renderer and only when null, so
// every property that changes what the map must BE has to drop it. Three do:
// setNumCascades, setShadowType and setShadowResolution. None of that is visible
// where it matters — the renderer simply finds a non-null map and renders into it,
// and the frame still comes out, just wrong: the resolution case renders a cascade
// into a fraction of a texture it is then sampled across, because the viewport and
// the shader's texel size both follow the new value while the texture does not.
//
// The second half is just as easy to lose. LightComponent::syncToLight pushes EVERY
// property onto the backing Light once per frame, so a setter that dropped the map
// unconditionally would reallocate it every frame forever. Each setter must be a
// no-op when the value is unchanged, and that is checked here too.

#include <iostream>
#include <memory>

#include "scene/light.h"
#include "scene/renderer/shadowMap.h"

using namespace visutwin::canvas;

namespace
{
    // A stand-in for an allocated map. createAtlasSlice does no device work — it
    // wraps whatever it is given — so the test needs no GraphicsDevice, which is
    // also how LightComponent builds its Light.
    void giveShadowMap(Light& light)
    {
        light.setShadowMap(ShadowMap::createAtlasSlice(nullptr, nullptr));
    }

    bool expectDropped(const char* what, Light& light)
    {
        if (light.shadowMap() != nullptr) {
            std::cerr << what << " kept the old shadow map; it is allocated once and"
                         " only when null, so the map now disagrees with the light\n";
            return false;
        }
        return true;
    }

    bool expectKept(const char* what, Light& light)
    {
        if (light.shadowMap() == nullptr) {
            std::cerr << what << " dropped the shadow map although nothing changed;"
                         " syncToLight replays every property each frame, so this"
                         " reallocates the map forever\n";
            return false;
        }
        return true;
    }

    bool checkResolution()
    {
        Light light(nullptr, false);
        light.setShadowResolution(2048);
        giveShadowMap(light);

        light.setShadowResolution(2048);
        if (!expectKept("setShadowResolution with an unchanged value", light)) {
            return false;
        }

        light.setShadowResolution(512);
        if (!expectDropped("setShadowResolution", light)) {
            return false;
        }
        if (light.shadowResolution() != 512) {
            std::cerr << "setShadowResolution did not store the new value\n";
            return false;
        }
        return true;
    }

    bool checkShadowType()
    {
        Light light(nullptr, false);
        light.setShadowType(SHADOW_PCF3_32F);
        giveShadowMap(light);

        light.setShadowType(SHADOW_PCF3_32F);
        if (!expectKept("setShadowType with an unchanged value", light)) {
            return false;
        }

        // PCF stores depth, VSM stores moments in a colour attachment — the map
        // cannot be reinterpreted, it has to be rebuilt.
        light.setShadowType(SHADOW_VSM_16F);
        return expectDropped("setShadowType", light);
    }

    bool checkNumCascades()
    {
        Light light(nullptr, false);
        light.setNumCascades(1);
        giveShadowMap(light);

        light.setNumCascades(1);
        if (!expectKept("setNumCascades with an unchanged value", light)) {
            return false;
        }

        light.setNumCascades(4);
        return expectDropped("setNumCascades", light);
    }

    // A light whose shadow is already considered rendered would never render into
    // the replacement map, leaving it blank for as long as the light lives.
    bool checkUpdateModeRearmed()
    {
        Light light(nullptr, false);
        light.setShadowResolution(2048);
        giveShadowMap(light);
        light.setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_NONE);

        light.setShadowResolution(1024);
        if (light.shadowUpdateMode() != ShadowUpdateType::SHADOWUPDATE_THISFRAME) {
            std::cerr << "dropping the shadow map left the light at SHADOWUPDATE_NONE,"
                         " so nothing will ever render into the new map\n";
            return false;
        }

        // A light that renders its shadow every frame must not be knocked off that.
        light.setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_REALTIME);
        light.setShadowResolution(256);
        if (light.shadowUpdateMode() != ShadowUpdateType::SHADOWUPDATE_REALTIME) {
            std::cerr << "dropping the shadow map downgraded a realtime shadow\n";
            return false;
        }
        return true;
    }
}

int main()
{
    const bool ok = checkResolution() && checkShadowType() && checkNumCascades() &&
        checkUpdateModeRearmed();
    if (!ok) {
        std::cerr << "shadow map invalidation tests FAILED\n";
        return 1;
    }
    std::cout << "shadow map invalidation tests passed\n";
    return 0;
}
