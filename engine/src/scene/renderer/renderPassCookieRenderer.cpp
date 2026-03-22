// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 01.10.2025.
//
#include "renderPassCookieRenderer.h"

namespace visutwin::canvas
{
    void RenderPassCookieRenderer::update(const std::vector<Light*>& lights) {
        // Pick lights we need to update the cookies for
        std::vector<Light*>& filteredLights = _filteredLights;
        filteredLights.clear();
        filter(lights, filteredLights);

        // Enable / disable the pass
        _executeEnabled = filteredLights.size() > 0;
    }

    void RenderPassCookieRenderer::filter(const std::vector<Light*>& lights, std::vector<Light*>& filteredLights)
    {
        for (auto* light : lights) {
            // Skip directional lights
            if (light->type() == LightType::LIGHTTYPE_DIRECTIONAL) {
                continue;
            }

            // Skip clustered cookies with no assigned atlas slot
            if (!light->atlasViewportAllocated()) {
                continue;
            }

            // Only render cookie when the slot is reassigned (assuming the cookie texture is static)
            if (!light->atlasSlotUpdated() && !_forceCopy) {
                continue;
            }

            if (light->enabled() && light->cookie() && light->visibleThisFrame()) {
                filteredLights.push_back(light);
            }
        }

        _forceCopy = false;
    }
}