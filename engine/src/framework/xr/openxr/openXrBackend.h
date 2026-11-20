// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 19.11.2026
//
#pragma once

#include <memory>
#include <string>

#include "framework/xr/xrBackend.h"

namespace visutwin::canvas
{
    struct OpenXrOptions
    {
        /// The name the runtime shows for the application.
        std::string applicationName = "VisuTwin Canvas";
        /// A handheld display (a phone or tablet) in place of a head-mounted one.
        bool handheld = false;
    };

    /**
     * The XR backend over OpenXR: a headset through whichever OpenXR runtime the system has
     * active. Null when the engine was built without it (VISUTWIN_XR_OPENXR, the "openxr"
     * vcpkg feature). Creating it starts nothing: the runtime is contacted when a session is
     * asked about or started, and a system with no runtime simply has no session to offer.
     *
     * The views are rendered straight into the runtime's swapchain images. Controllers are
     * read through one action set with bindings for the common interaction profiles; the
     * depth slices of a session are submitted as projection layers, farthest first.
     */
    std::shared_ptr<XrBackend> createOpenXrBackend(const OpenXrOptions& options = {});
}
