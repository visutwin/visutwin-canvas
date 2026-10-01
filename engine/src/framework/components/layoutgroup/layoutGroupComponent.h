// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Lays out the element children of its entity (upstream framework/components/layout-group/
// component.js): in a row or a column, wrapping into more of them when asked, sized to the
// group by the width and height fittings and each child's LayoutChildComponent, and placed
// by the alignment, padding and spacing. A child takes part while its entity and its element
// are enabled and its layout child does not exclude it. The group sets each child's anchors
// to zero, its calculated size and its local position; the reflow event then carries the
// bounds of what was laid out:
//
//     group->on("reflow", [](const Vector4& bounds) { ... });   // x, y, width, height
//
// DEVIATION: upstream reflows when one of a dozen events says an input changed (a child
// inserted, resized, enabled ...). Here the layout group system compares every group's
// inputs after each update and reflows the groups whose inputs differ from their last
// reflow, outermost first, until none does. A reflow is a pure function of those inputs, so
// the layouts are upstream's; nothing has to fire an event this port does not have.
//
#pragma once

#include <cstdint>
#include <vector>

#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/components/component.h"
#include "layoutCalculator.h"
#include "framework/components/componentInstanceList.h"

namespace visutwin::canvas
{
    class LayoutGroupComponent : public Component
    {
    public:
        LayoutGroupComponent(IComponentSystem* system, Entity* entity);
        ~LayoutGroupComponent() override;

        void initializeComponentData() override {}
        void cloneFrom(const Component* source) override;
        void onDisable() override { _lastInputs.clear(); }

        static const std::vector<LayoutGroupComponent*>& instances() { return _instanceList.items(); }

        Orientation orientation() const { return _options.orientation; }
        void setOrientation(const Orientation value) { _options.orientation = value; }
        bool reverseX() const { return _options.reverseX; }
        void setReverseX(const bool value) { _options.reverseX = value; }
        /// On by default, as upstream: rows stack from the top.
        bool reverseY() const { return _options.reverseY; }
        void setReverseY(const bool value) { _options.reverseY = value; }
        /// Where the laid-out block sits: x 0 left to 1 right, y 0 bottom to 1 top. Default (0, 1).
        const Vector2& alignment() const { return _options.alignment; }
        void setAlignment(const Vector2& value) { _options.alignment = value; }
        /// Left, bottom, right, top.
        const Vector4& padding() const { return _options.padding; }
        void setPadding(const Vector4& value) { _options.padding = value; }
        const Vector2& spacing() const { return _options.spacing; }
        void setSpacing(const Vector2& value) { _options.spacing = value; }
        LayoutFitting widthFitting() const { return _options.widthFitting; }
        void setWidthFitting(const LayoutFitting value) { _options.widthFitting = value; }
        LayoutFitting heightFitting() const { return _options.heightFitting; }
        void setHeightFitting(const LayoutFitting value) { _options.heightFitting = value; }
        bool wrap() const { return _options.wrap; }
        void setWrap(const bool value) { _options.wrap = value; }

        /// Lay the children out now (upstream `reflow`), and fire `reflow` with the bounds.
        void reflow();
        /// Reflow if an input changed since the last reflow; returns whether it did. The
        /// system calls this after each update.
        bool reflowIfChanged();

    private:
        /// Everything a reflow reads, as bits so the comparison is exact (NaN included): the
        /// options, the group's size, and each child's identity, state, size, pivot, anchors
        /// and layout-child settings.
        /// Written into `inputs`, which keeps its capacity: this runs for every group
        /// after every update, and must not allocate when nothing changed.
        void gatherInputs(std::vector<uint32_t>& inputs) const;

        inline static ComponentInstanceList<LayoutGroupComponent> _instanceList;
        LayoutOptions _options;
        std::vector<uint32_t> _lastInputs;
        // Scratch for the comparison; swapped with _lastInputs when they differ.
        std::vector<uint32_t> _currentInputs;
    };
}
