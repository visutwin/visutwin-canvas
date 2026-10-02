// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The SSAO key bindings the two ambient-occlusion examples share, so that each SSAO
// parameter's effect can be isolated at runtime: O toggles SSAO, T switches lighting /
// combine (where offered), B the blur, Z the randomization, +/- the intensity, [/] the
// radius, ,/. the sample count and ;/' the power. Every change logs the new state.
//
#pragma once

#include <algorithm>
#include <string>

#include <SDL3/SDL.h>

#include "core/log.h"
#include "framework/components/camera/cameraComponent.h"
#include "scene/graphics/renderPassConstants.h"

namespace visutwin::canvas
{
    struct SsaoKeyOptions
    {
        /// Bind T to the lighting / combine switch, and log the type.
        bool typeKey = true;
    };

    inline void logSsaoState(const CameraComponent* camera, const char* reason, const SsaoKeyOptions options = {})
    {
        if (!camera) {
            return;
        }
        const auto& ssao = camera->ssao();
        if (options.typeKey) {
            spdlog::info("SSAO {}: enabled={}, type={}, blur={}, intensity={:.2f}, power={:.1f}, radius={:.1f}, samples={}, minAngle={:.1f}, scale={:.2f}, randomize={}",
                reason,
                ssao.enabled ? "ON" : "OFF",
                std::string(ssao.type),
                ssao.blurEnabled ? "ON" : "OFF",
                ssao.intensity,
                ssao.power,
                ssao.radius,
                ssao.samples,
                ssao.minAngle,
                ssao.scale,
                ssao.randomize ? "ON" : "OFF");
        } else {
            spdlog::info("SSAO {}: enabled={}, blur={}, intensity={:.2f}, power={:.1f}, radius={:.1f}, samples={}, minAngle={:.1f}, scale={:.2f}, randomize={}",
                reason,
                ssao.enabled ? "ON" : "OFF",
                ssao.blurEnabled ? "ON" : "OFF",
                ssao.intensity,
                ssao.power,
                ssao.radius,
                ssao.samples,
                ssao.minAngle,
                ssao.scale,
                ssao.randomize ? "ON" : "OFF");
        }
    }

    /// Applies the SSAO binding of `key` to `camera` and logs the result. Returns false,
    /// changing nothing, for a key that is not an SSAO binding.
    inline bool handleSsaoKey(CameraComponent* camera, const SDL_Keycode key, const SsaoKeyOptions options = {})
    {
        if (!camera) {
            return false;
        }
        auto ssao = camera->ssao();
        const char* reason = nullptr;
        switch (key) {
        case SDLK_O:
            ssao.enabled = !ssao.enabled;
            reason = "toggle";
            break;
        case SDLK_B:
            ssao.blurEnabled = !ssao.blurEnabled;
            reason = "blur";
            break;
        case SDLK_Z:
            ssao.randomize = !ssao.randomize;
            reason = "randomize";
            break;
        case SDLK_T:
            if (!options.typeKey) {
                return false;
            }
            ssao.type = (ssao.type == SSAOTYPE_LIGHTING) ? SSAOTYPE_COMBINE : SSAOTYPE_LIGHTING;
            reason = "type";
            break;
        case SDLK_EQUALS:
            ssao.intensity = std::min(1.0f, ssao.intensity + 0.05f);
            reason = "intensity+";
            break;
        case SDLK_MINUS:
            ssao.intensity = std::max(0.0f, ssao.intensity - 0.05f);
            reason = "intensity-";
            break;
        case SDLK_RIGHTBRACKET:
            ssao.radius = std::min(100.0f, ssao.radius + 5.0f);
            reason = "radius+";
            break;
        case SDLK_LEFTBRACKET:
            ssao.radius = std::max(1.0f, ssao.radius - 5.0f);
            reason = "radius-";
            break;
        case SDLK_PERIOD:
            ssao.samples = std::min(32, ssao.samples + 2);
            reason = "samples+";
            break;
        case SDLK_COMMA:
            ssao.samples = std::max(2, ssao.samples - 2);
            reason = "samples-";
            break;
        case SDLK_APOSTROPHE:
            ssao.power = std::min(16.0f, ssao.power + 1.0f);
            reason = "power+";
            break;
        case SDLK_SEMICOLON:
            ssao.power = std::max(0.5f, ssao.power - 1.0f);
            reason = "power-";
            break;
        default:
            return false;
        }
        camera->setSsao(ssao);
        logSsaoState(camera, reason, options);
        return true;
    }
}
