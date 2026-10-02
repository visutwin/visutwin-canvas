// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 02.10.2026
//
// The element builder the UI examples share: the stand-in for upstream's
// `entity.addComponent('element', {...})`, whose data object ElementProps mirrors.
//
// A field left unset keeps the element's own default, which is upstream's. An example
// whose elements share a look (a light colour, a centred anchor, its font) passes that
// once as `defaults`: every field a call leaves unset is taken from it, so the call sites
// name only what differs, as upstream's do.
//
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/math/vector4.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementComponentSystem.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/sprite.h"

namespace visutwin::canvas
{
    /// The element properties the examples set. Unset fields keep the element's defaults:
    /// a group, white, anchored and pivoted at the parent's bottom-left corner and centre.
    struct ElementProps
    {
        std::optional<ElementType> type;
        std::shared_ptr<Sprite> sprite;
        int spriteFrame = 0;
        Texture* texture = nullptr;
        /// A custom material that draws an image element's quad instead of its own.
        std::shared_ptr<Material> material;
        std::optional<ElementFitMode> fitMode;
        std::optional<Color> color;
        std::optional<float> opacity;
        std::optional<Vector4> anchor;
        std::optional<Vector2> pivot;
        std::optional<Vector4> margin;
        std::optional<float> width;
        std::optional<float> height;
        bool useInput = false;
        std::optional<bool> mask;
        /// A text element's font; unset, the defaults' font.
        FontResource* font = nullptr;
        std::string text;
        /// A localization key, set instead of `text` when not empty.
        std::string key;
        std::optional<int> fontSize;
        std::optional<float> lineHeight;
        std::optional<bool> autoWidth;
        std::optional<bool> wrapLines;
        std::optional<bool> enableMarkup;
        std::optional<ElementHorizontalAlign> horizontalAlign;
        std::optional<float> verticalAlign;
    };

    /// `props` with every setting it leaves unset taken from `defaults`. What the element
    /// shows (sprite, texture, material, text) and useInput are the call's own.
    inline ElementProps withElementDefaults(ElementProps props, const ElementProps& defaults)
    {
        const auto fill = [](auto& field, const auto& fallback) {
            if (!field) {
                field = fallback;
            }
        };
        fill(props.type, defaults.type);
        fill(props.fitMode, defaults.fitMode);
        fill(props.color, defaults.color);
        fill(props.opacity, defaults.opacity);
        fill(props.anchor, defaults.anchor);
        fill(props.pivot, defaults.pivot);
        fill(props.margin, defaults.margin);
        fill(props.width, defaults.width);
        fill(props.height, defaults.height);
        fill(props.mask, defaults.mask);
        fill(props.font, defaults.font);
        fill(props.fontSize, defaults.fontSize);
        fill(props.lineHeight, defaults.lineHeight);
        fill(props.autoWidth, defaults.autoWidth);
        fill(props.wrapLines, defaults.wrapLines);
        fill(props.enableMarkup, defaults.enableMarkup);
        fill(props.horizontalAlign, defaults.horizontalAlign);
        fill(props.verticalAlign, defaults.verticalAlign);
        return props;
    }

    /// An element entity under `parent`, built from `props` (unset fields from `defaults`).
    ///
    /// The layout goes through ElementComponent::setup first. A text's settings are all in place before its font, so the text is laid
    /// out once they are: autoWidth in particular is off before the first layout, which
    /// would otherwise have sized a fixed-width text to the empty string.
    inline ElementComponent* createElement(Engine* engine, Entity* parent, const ElementProps& elementProps,
                                           const ElementProps& defaults = {})
    {
        const ElementProps props = withElementDefaults(elementProps, defaults);

        auto* entity = new Entity();
        entity->setEngine(engine);
        auto* element = static_cast<ElementComponent*>(entity->addComponent<ElementComponent>());
        ElementDesc desc{.type = props.type, .anchor = props.anchor, .pivot = props.pivot, .margin = props.margin};
        desc.width = props.width;
        desc.height = props.height;
        desc.useInput = props.useInput;
        element->setup(desc);

        if (props.sprite) {
            element->setSprite(props.sprite);
            element->setSpriteFrame(props.spriteFrame);
        }
        if (props.texture) {
            element->setTexture(props.texture);
        }
        if (props.material) {
            element->setMaterial(props.material);
        }
        if (props.fitMode) {
            element->setFitMode(*props.fitMode);
        }
        if (props.color) {
            element->setColor(*props.color);
        }
        if (props.opacity) {
            element->setOpacity(*props.opacity);
        }
        if (props.mask) {
            element->setMask(*props.mask);
        }

        if (element->type() == ElementType::Text) {
            if (props.autoWidth) {
                element->setAutoWidth(*props.autoWidth);
            }
            if (props.wrapLines) {
                element->setWrapLines(*props.wrapLines);
            }
            if (props.enableMarkup) {
                element->setEnableMarkup(*props.enableMarkup);
            }
            if (props.horizontalAlign) {
                element->setHorizontalAlign(*props.horizontalAlign);
            }
            if (props.verticalAlign) {
                element->setVerticalAlign(*props.verticalAlign);
            }
            if (props.lineHeight) {
                element->setLineHeight(*props.lineHeight);
            }
            element->setFontResource(props.font);
            if (props.fontSize) {
                element->setFontSize(*props.fontSize);
            }
            if (!props.key.empty()) {
                element->setKey(props.key);
            } else {
                element->setText(props.text);
            }
        }

        parent->addChild(entity);
        return element;
    }
}
