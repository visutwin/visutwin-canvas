// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// How a layout group may size one of its children (upstream framework/components/
// layout-child/component.js): limits on its width and height, its share of the space a
// Stretch or Shrink fit hands out, and whether it takes part at all. The group reads these
// whenever it lays its children out; a child without this component takes part with its own
// width and height and no limits.
//
#pragma once

#include <optional>

#include "framework/components/component.h"

namespace visutwin::canvas
{
    class LayoutChildComponent : public Component
    {
    public:
        LayoutChildComponent(IComponentSystem* system, Entity* entity) : Component(system, entity) {}

        void initializeComponentData() override {}
        void cloneFrom(const Component* source) override
        {
            if (const auto* src = dynamic_cast<const LayoutChildComponent*>(source)) {
                _minWidth = src->_minWidth;
                _minHeight = src->_minHeight;
                _maxWidth = src->_maxWidth;
                _maxHeight = src->_maxHeight;
                _fitWidthProportion = src->_fitWidthProportion;
                _fitHeightProportion = src->_fitHeightProportion;
                _excludeFromLayout = src->_excludeFromLayout;
            }
        }

        float minWidth() const { return _minWidth; }
        void setMinWidth(const float value) { _minWidth = value; }
        float minHeight() const { return _minHeight; }
        void setMinHeight(const float value) { _minHeight = value; }
        /// No value is no limit (upstream's null).
        std::optional<float> maxWidth() const { return _maxWidth; }
        void setMaxWidth(const std::optional<float> value) { _maxWidth = value; }
        std::optional<float> maxHeight() const { return _maxHeight; }
        void setMaxHeight(const std::optional<float> value) { _maxHeight = value; }
        /// The child's share of the space a fit adds or takes away; all zero shares equally.
        float fitWidthProportion() const { return _fitWidthProportion; }
        void setFitWidthProportion(const float value) { _fitWidthProportion = value; }
        float fitHeightProportion() const { return _fitHeightProportion; }
        void setFitHeightProportion(const float value) { _fitHeightProportion = value; }
        /// Left out of the layout: the group neither sizes nor places it.
        bool excludeFromLayout() const { return _excludeFromLayout; }
        void setExcludeFromLayout(const bool value) { _excludeFromLayout = value; }

    private:
        float _minWidth = 0.0f;
        float _minHeight = 0.0f;
        std::optional<float> _maxWidth;
        std::optional<float> _maxHeight;
        float _fitWidthProportion = 0.0f;
        float _fitHeightProportion = 0.0f;
        bool _excludeFromLayout = false;
    };
}
