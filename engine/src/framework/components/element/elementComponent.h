// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/components/component.h"
#include "framework/handlers/fontResource.h"

namespace visutwin::canvas
{
    enum class ElementType
    {
        Image,
        Text
    };

    enum class ElementHorizontalAlign
    {
        Left,
        Center,
        Right
    };

    class ElementComponent : public Component
    {
    public:
        ElementComponent(IComponentSystem* system, Entity* entity);
        ~ElementComponent() override;

        void initializeComponentData() override {}

        static const std::vector<ElementComponent*>& instances() { return _instances; }

        ElementType type() const { return _type; }
        void setType(const ElementType value) { _type = value; }

        const Vector2& pivot() const { return _pivot; }
        void setPivot(const Vector2& value) { _pivot = value; _textDirty = true; }

        const Vector4& anchor() const { return _anchor; }
        void setAnchor(const Vector4& value) { _anchor = value; _textDirty = true; }

        const Vector4& margin() const { return _margin; }
        void setMargin(const Vector4& value) { _margin = value; _textDirty = true; }

        float width() const { return _width; }
        void setWidth(const float value) { _width = std::max(value, 0.0f); _textDirty = true; }

        float height() const { return _height; }
        void setHeight(const float value) { _height = std::max(value, 0.0f); _textDirty = true; }

        float opacity() const { return _opacity; }
        void setOpacity(const float value) { _opacity = std::clamp(value, 0.0f, 1.0f); }

        const Color& color() const { return _color; }
        void setColor(const Color& value) { _color = value; }

        int fontSize() const { return _fontSize; }
        void setFontSize(const int value) { _fontSize = std::max(value, 1); _textDirty = true; }

        const std::string& text() const { return _text; }
        void setText(const std::string& value) { _text = value; _textDirty = true; }

        FontResource* fontResource() const { return _fontResource; }
        void setFontResource(FontResource* value) { _fontResource = value; _textDirty = true; }

        ElementHorizontalAlign horizontalAlign() const { return _horizontalAlign; }
        void setHorizontalAlign(const ElementHorizontalAlign value) { _horizontalAlign = value; _textDirty = true; }

        bool wrapLines() const { return _wrapLines; }
        void setWrapLines(const bool value) { _wrapLines = value; _textDirty = true; }

        bool useInput() const { return _useInput; }
        void setUseInput(const bool value) { _useInput = value; }

        bool textDirty() const { return _textDirty; }
        void clearTextDirty() { _textDirty = false; }

    private:
        inline static std::vector<ElementComponent*> _instances;

        ElementType _type = ElementType::Image;
        Vector2 _pivot = Vector2(0.5f, 0.5f);
        Vector4 _anchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector4 _margin = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        float _width = 100.0f;
        float _height = 50.0f;
        float _opacity = 1.0f;
        Color _color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        int _fontSize = 16;
        std::string _text;
        FontResource* _fontResource = nullptr;
        ElementHorizontalAlign _horizontalAlign = ElementHorizontalAlign::Center;
        bool _wrapLines = false;
        bool _textDirty = true;
        bool _useInput = false;
    };
}
