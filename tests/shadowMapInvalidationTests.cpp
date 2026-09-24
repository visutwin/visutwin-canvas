// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A Light's shadow map is allocated lazily by the renderer and only when null, so
// every property that changes what the map must BE — or makes it needed at all —
// has to drop it. Four do: setNumCascades, setShadowType, setShadowResolution and
// setCastShadows. None of that is visible
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
    // A stand-in for an allocated map. createAtlas does no device work — it
    // wraps whatever it is given — so the test needs no GraphicsDevice, which is
    // also how LightComponent builds its Light.
    void giveShadowMap(Light& light)
    {
        light.setShadowMap(ShadowMap::createAtlas(nullptr, nullptr));
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
        Light light(nullptr);
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
        Light light(nullptr);
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
        Light light(nullptr);
        light.setNumCascades(1);
        giveShadowMap(light);

        light.setNumCascades(1);
        if (!expectKept("setNumCascades with an unchanged value", light)) {
            return false;
        }

        light.setNumCascades(4);
        return expectDropped("setNumCascades", light);
    }

    bool checkCastShadows()
    {
        Light light(nullptr);
        // castShadows() folds the mask in, and a bare Light defaults to MASK_NONE,
        // which would make the getter false whatever setCastShadows said. A
        // LightComponent pushes its own mask (MASK_AFFECT_DYNAMIC) every frame, so
        // this stands in for that. NOTE: upstream's Light defaults the mask to
        // MASK_AFFECT_DYNAMIC; this port does not, which is a defaults divergence
        // separate from what this file is about.
        light.setMask(MaskType::MASK_AFFECT_DYNAMIC);
        light.setCastShadows(true);
        giveShadowMap(light);

        light.setCastShadows(true);
        if (!expectKept("setCastShadows with an unchanged value", light)) {
            return false;
        }

        // Switching shadows off leaves nothing that reads the map, so holding it is
        // pure waste for the life of the light.
        light.setCastShadows(false);
        if (!expectDropped("setCastShadows(false)", light)) {
            return false;
        }

        // And off-to-off must not keep re-dropping: syncToLight replays it every
        // frame, and destroyShadowMap re-arms the update mode each time it runs.
        light.setShadowUpdateMode(ShadowUpdateType::SHADOWUPDATE_NONE);
        light.setCastShadows(false);
        if (light.shadowUpdateMode() != ShadowUpdateType::SHADOWUPDATE_NONE) {
            std::cerr << "setCastShadows(false) on a light that already had shadows off"
                         " re-armed the update mode, so a static shadow would re-render"
                         " every frame\n";
            return false;
        }

        // Turning them back on has to get a fresh map rather than the null it was
        // left with — the renderer allocates only when null, so this is the path
        // that would otherwise leave the light shadowless after a toggle.
        light.setCastShadows(true);
        if (light.shadowMap() != nullptr) {
            std::cerr << "setCastShadows(true) did not leave the map null for the"
                         " renderer to allocate\n";
            return false;
        }
        if (!light.castShadows()) {
            std::cerr << "setCastShadows(true) did not store the new value\n";
            return false;
        }
        return true;
    }

    // A light whose shadow is already considered rendered would never render into
    // the replacement map, leaving it blank for as long as the light lives.
    bool checkUpdateModeRearmed()
    {
        Light light(nullptr);
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

    // VSM is directional-only in this port: a spot or omni light asking for it gets
    // PCF3 (it used to keep VSM and come out unshadowed), and the resolution follows
    // the light TYPE, so changing the type re-resolves the kept request.
    bool checkLocalVsmFallsBack()
    {
        Light light(nullptr);
        light.setShadowType(SHADOW_VSM_16F);
        if (light.shadowType() != SHADOW_VSM_16F) {
            std::cerr << "a directional light asking for VSM did not get it\n";
            return false;
        }
        giveShadowMap(light);
        light.setType(LightType::LIGHTTYPE_SPOT);
        if (light.shadowType() != SHADOW_PCF3_32F || light.requestedShadowType() != SHADOW_VSM_16F) {
            std::cerr << "a spot light asking for VSM did not fall back to PCF3 (or lost the request)\n";
            return false;
        }
        if (!expectDropped("setType(SPOT)", light)) {
            return false;
        }
        giveShadowMap(light);
        light.setType(LightType::LIGHTTYPE_SPOT);
        if (!expectKept("setType(SPOT) again", light)) {
            return false;
        }
        light.setType(LightType::LIGHTTYPE_OMNI);
        if (light.shadowType() != SHADOW_PCF3_32F) {
            std::cerr << "an omni light asking for VSM did not fall back to PCF3\n";
            return false;
        }
        light.setType(LightType::LIGHTTYPE_DIRECTIONAL);
        if (light.shadowType() != SHADOW_VSM_16F) {
            std::cerr << "back to directional, the kept VSM request was not honoured again\n";
            return false;
        }
        // Setting VSM on a light that is ALREADY a spot resolves at once too.
        Light spot(nullptr);
        spot.setType(LightType::LIGHTTYPE_SPOT);
        spot.setShadowType(SHADOW_VSM_16F);
        if (spot.shadowType() != SHADOW_PCF3_32F) {
            std::cerr << "VSM requested on an existing spot light was not resolved to PCF3\n";
            return false;
        }
        return true;
    }
}

int main()
{
    const bool ok = checkResolution() && checkShadowType() && checkNumCascades() &&
        checkCastShadows() && checkUpdateModeRearmed() && checkLocalVsmFallsBack();
    if (!ok) {
        std::cerr << "shadow map invalidation tests FAILED\n";
        return 1;
    }
    std::cout << "shadow map invalidation tests passed\n";
    return 0;
}
