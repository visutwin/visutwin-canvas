// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "elementComponent.h"

#include <algorithm>

namespace visutwin::canvas
{
    ElementComponent::ElementComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    ElementComponent::~ElementComponent()
    {
        std::erase(_instances, this);
    }

    void ElementComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const ElementComponent*>(source);
        if (!src) {
            return;
        }
        _type = src->_type;
        _pivot = src->_pivot;
        _anchor = src->_anchor;
        _margin = src->_margin;
        _width = src->_width;
        _height = src->_height;
        _opacity = src->_opacity;
        _color = src->_color;
        _fontSize = src->_fontSize;
        _text = src->_text;
        _fontResource = src->_fontResource;
        _horizontalAlign = src->_horizontalAlign;
        _wrapLines = src->_wrapLines;
        _useInput = src->_useInput;
        _textDirty = true;   // the clone has no text mesh of its own yet
    }
}
