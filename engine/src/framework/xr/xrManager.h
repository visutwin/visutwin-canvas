// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025.
//
#pragma once

#include "core/eventHandler.h"

namespace visutwin::canvas
{
    using XrErrorCallback = std::function<void(const std::string* err)>;

    using MakeTickCallback = std::function<void(double, void*)>;

    /**
     * XrManager provides a comprehensive interface for XR integration in VisuTwin applications
     */
    class XrManager : public EventHandler
    {
    public:
        bool active() const { return _session != nullptr; }

        // Attempts to end the XR session and optionally fires callback when the session is ended or failed to end
        void end(XrErrorCallback callback = nullptr) {}

        bool update(void* frame) { return _session != nullptr && frame != nullptr; }

        void* requestAnimationFrame(const MakeTickCallback& callback) { return nullptr; }

    private:
        void* _session = nullptr;
    };
}
