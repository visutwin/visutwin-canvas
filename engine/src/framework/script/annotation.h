// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
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
     * - `annotation:add` — when the annotation initializes
     * - `annotation:remove` — when the annotation is destroyed
     *
     * Fires the following script-level events (listened to by AnnotationManager):
     * - `label:set` — when label changes
     * - `title:set` — when title changes
     * - `text:set` — when text changes
     * - `hover` — when hover state changes
     * - `show` — when tooltip is shown
     * - `hide` — when tooltip is hidden
     */
    class Annotation : public Script
    {
    public:
        SCRIPT_NAME("annotation")

        // Expose entity() as public so AnnotationManager can access the owning entity
        using Script::entity;

        const std::string& label() const { return _label; }
        void setLabel(const std::string& value)
        {
            _label = value;
            fire("label:set", value);
        }

        const std::string& title() const { return _title; }
        void setTitle(const std::string& value)
        {
            _title = value;
            fire("title:set", value);
        }

        const std::string& text() const { return _text; }
        void setText(const std::string& value)
        {
            _text = value;
            fire("text:set", value);
        }

        /**
         * Call after setting label/title/text to register with the AnnotationManager.
         * Must be called explicitly because properties need to be set before registration
         * (the hotspot texture is generated from the label at registration time).
         */
        void activate();

    private:
        std::string _label;
        std::string _title;
        std::string _text;
    };
}

REGISTER_SCRIPT(visutwin::canvas::Annotation, "annotation")
