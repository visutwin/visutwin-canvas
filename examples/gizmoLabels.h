// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 24.10.2026
//
// Draws the labels a gizmo leaves each frame (GizmoReadout: a transform gizmo's readout, a
// handle gizmo's handle values) as UI text on a near-black panel, centred on the label's x
// with its top edge at y, in 12-point text with the labels' colours.
//
// DEVIATIONS: upstream's are HTML elements in 12px monospace with bold labels; here they are UI
// text elements in Roboto, one weight. Roboto has no θ or ⌀, so a label's θ shows as A (an
// angle) and its ⌀ as Ø.
//
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "framework/components/element/elementComponent.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/gizmo/gizmoReadout.h"
#include "uiElements.h"

namespace visutwin::canvas
{
    class GizmoLabels
    {
    public:
        /// Labels on a screen measured in canvas points (scale mode none), in `font`.
        GizmoLabels(Engine* engine, ScreenComponent* screen, FontResource* font)
            : _engine(engine), _screen(screen), _font(font)
        {
        }

        /// Shows these labels, and hides the ones shown before that are not among them.
        void update(const std::vector<GizmoReadout>& labels)
        {
            size_t shown = 0;
            for (const GizmoReadout& readout : labels) {
                if (!readout.visible || !_screen || !_font) {
                    continue;
                }
                Label& label = take(shown++);
                label.panel->entity()->setEnabled(true);
                const std::string markup = drawable(readout.markup);
                if (markup != label.markup) {
                    label.markup = markup;
                    label.text->setText(markup);
                }
                // upstream's padding: 2 points above and below, 6 at the sides
                label.panel->setWidth(label.text->width() + 12.0f);
                label.panel->setHeight(label.text->height() + 4.0f);
                label.panel->entity()->setLocalPosition(readout.x, -readout.y, 0.0f);
            }
            for (size_t i = shown; i < _labels.size(); ++i) {
                _labels[i].panel->entity()->setEnabled(false);
            }
        }

        /// Shows one label, or none.
        void update(const GizmoReadout& readout)
        {
            update(std::vector<GizmoReadout>{readout});
        }

    private:
        struct Label
        {
            ElementComponent* panel = nullptr;
            ElementComponent* text = nullptr;
            std::string markup;
        };

        // A panel anchored at the screen's top-left corner, its pivot the top of its centre, so
        // a label's position places it directly.
        Label& take(const size_t index)
        {
            while (_labels.size() <= index) {
                Label label;
                label.panel = createElement(_engine, _screen->entity(), {.type = ElementType::Image,
                    .color = Color(0.0f, 0.0f, 0.0f, 1.0f), .opacity = 0.9f, .anchor = Vector4(0.0f, 1.0f, 0.0f, 1.0f),
                    .pivot = Vector2(0.5f, 1.0f), .width = 10.0f, .height = 10.0f});
                label.text = createElement(_engine, label.panel->entity(), {.type = ElementType::Text,
                    .color = Color(1.0f, 1.0f, 1.0f, 1.0f), .anchor = Vector4(0.5f, 0.5f, 0.5f, 0.5f),
                    .pivot = Vector2(0.5f, 0.5f), .font = _font, .fontSize = 12, .enableMarkup = true});
                _labels.push_back(label);
            }
            return _labels[index];
        }

        // The markup with the characters the font lacks replaced.
        static std::string drawable(std::string markup)
        {
            const auto replace = [&markup](const std::string& from, const std::string& to) {
                for (size_t at = markup.find(from); at != std::string::npos; at = markup.find(from, at + to.size())) {
                    markup.replace(at, from.size(), to);
                }
            };
            replace("θ", "A");
            replace("⌀", "Ø");
            return markup;
        }

        Engine* _engine = nullptr;
        ScreenComponent* _screen = nullptr;
        FontResource* _font = nullptr;
        std::vector<Label> _labels;
    };
}
