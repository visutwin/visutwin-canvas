// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
#pragma once

#include <string>

#include "script.h"
#include "scriptRegistry.h"

namespace visutwin::canvas
{
    class Engine;

    /**
     * A lightweight data script for creating interactive 3D annotations in a scene.
     * This script only holds the annotation data (label, title, text) — all rendering
     * and interaction is handled by an AnnotationManager listening for engine events.
     *
     * Fires the following engine-level events:
     * - `annotation:add` (Annotation*) — when the annotation post-initializes
     * - `annotation:remove` (Annotation*) — when the annotation is destroyed
     *
     * Fires the following script-level events (listened to by AnnotationManager):
     * - `label:set` — when label changes
     * - `title:set` — when title changes
     * - `text:set` — when text changes
     * - `hover` (bool) — when hover state changes
     * - `show` (Annotation*) — when tooltip is shown
     * - `hide` — when tooltip is hidden
     */
    class Annotation : public Script
    {
    public:
        SCRIPT_NAME("annotation")

        // Public so the AnnotationManager can reach the owning entity.
        using Script::entity;

        /// The short text displayed on the hotspot circle (e.g. "1", "A").
        const std::string& label() const { return _label; }
        void setLabel(const std::string& value)
        {
            _label = value;
            fire("label:set", value);
        }

        /// The title shown in the tooltip when the hotspot is clicked.
        const std::string& title() const { return _title; }
        void setTitle(const std::string& value)
        {
            _title = value;
            fire("title:set", value);
        }

        /// The description text shown in the tooltip when the hotspot is clicked.
        const std::string& text() const { return _text; }
        void setText(const std::string& value)
        {
            _text = value;
            fire("text:set", value);
        }

        /// Called after every script has initialized, so the AnnotationManager is ready
        /// to receive the `annotation:add` event.
        void postInitialize() override;

    private:
        std::string _label;
        std::string _title;
        std::string _text;
    };
}

REGISTER_SCRIPT(visutwin::canvas::Annotation, "annotation")
