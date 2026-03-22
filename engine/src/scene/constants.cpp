// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.12.2025.
//
#include "constants.h"

namespace visutwin::canvas
{
    const std::unordered_map<ShadowType, ShadowTypeInfo> shadowTypeInfo = {
        { SHADOW_PCF1_32F, { "PCF1_32F", "PCF1", PixelFormat::PIXELFORMAT_DEPTH, .pcf = true }},
        { SHADOW_PCF3_32F, { "PCF3_32F", "PCF3", PixelFormat::PIXELFORMAT_DEPTH, .pcf = true }},
        /*SHADOW_PCF5_32F, { name: 'PCF5_32F', kind: 'PCF5', format: PIXELFORMAT_DEPTH, pcf: true },
        SHADOW_PCF1_16F, { name: 'PCF1_16F', kind: 'PCF1', format: PIXELFORMAT_DEPTH16, pcf: true },
        SHADOW_PCF3_16F, { name: 'PCF3_16F', kind: 'PCF3', format: PIXELFORMAT_DEPTH16, pcf: true },
        SHADOW_PCF5_16F, { name: 'PCF5_16F', kind: 'PCF5', format: PIXELFORMAT_DEPTH16, pcf: true },*/
        { SHADOW_VSM_16F, { "VSM_16F", "VSM", PixelFormat::PIXELFORMAT_RGBA16F, .vsm = true }},
        /*SHADOW_VSM_32F, { name: 'VSM_32F', kind: 'VSM', format: PIXELFORMAT_RGBA32F, vsm: true },
        SHADOW_PCSS_32F, { name: 'PCSS_32F', kind: 'PCSS', format: PIXELFORMAT_R32F, pcss: true }*/
    };
}