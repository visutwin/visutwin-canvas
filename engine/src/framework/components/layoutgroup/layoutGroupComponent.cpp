// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
#include "layoutGroupComponent.h"

#include "framework/components/componentRegistry.h"

#include <algorithm>
#include <bit>
#include <limits>

#include "framework/components/element/elementComponent.h"
#include "framework/components/layoutchild/layoutChildComponent.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    namespace
    {
        /// The child's element, when the child takes part (before the layout
        /// child's exclusion).
        ElementComponent* enabledElementOf(GraphNode* node)
        {
            auto* entity = dynamic_cast<Entity*>(node);
            if (!entity || !entity->enabled()) {
                return nullptr;
            }
            auto* element = entity->findComponent<ElementComponent>();
            return element && element->enabled() ? element : nullptr;
        }

        /// The child's layout child, when it is enabled (a disabled one reads as absent).
        LayoutChildComponent* enabledLayoutChildOf(Entity* entity)
        {
            auto* child = entity->findComponent<LayoutChildComponent>();
            return child && child->enabled() ? child : nullptr;
        }
    }

    LayoutGroupComponent::LayoutGroupComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        listInstance(this);
    }

    LayoutGroupComponent::~LayoutGroupComponent()
    {
        unlistInstance();
    }

    void LayoutGroupComponent::cloneFrom(const Component* source)
    {
        if (const auto* src = dynamic_cast<const LayoutGroupComponent*>(source)) {
            _options = src->_options;
        }
    }

    void LayoutGroupComponent::gatherInputs(std::vector<uint32_t>& inputs) const
    {
        inputs.clear();
        const auto push = [&inputs](const float value) { inputs.push_back(std::bit_cast<uint32_t>(value)); };
        const auto pushSerial = [&inputs](const uint64_t serial) {
            inputs.push_back(static_cast<uint32_t>(serial));
            inputs.push_back(static_cast<uint32_t>(serial >> 32));
        };

        inputs.push_back(static_cast<uint32_t>(_options.orientation));
        inputs.push_back((_options.reverseX ? 1u : 0u) | (_options.reverseY ? 2u : 0u) | (_options.wrap ? 4u : 0u));
        inputs.push_back(static_cast<uint32_t>(_options.widthFitting));
        inputs.push_back(static_cast<uint32_t>(_options.heightFitting));
        push(_options.alignment.x);
        push(_options.alignment.y);
        push(_options.padding.getX());
        push(_options.padding.getY());
        push(_options.padding.getZ());
        push(_options.padding.getW());
        push(_options.spacing.x);
        push(_options.spacing.y);

        const auto* container = _entity->findComponent<ElementComponent>();
        inputs.push_back(container ? 1u : 0u);
        if (container) {
            push(container->calculatedWidth());
            push(container->calculatedHeight());
        }

        for (const auto& child : _entity->children()) {
            ElementComponent* element = enabledElementOf(child.get());
            if (!element) {
                continue;
            }
            // A serial, not the address: a new child must be placed even where it reuses a
            // freed one's memory.
            pushSerial(element->serial());
            push(element->width());
            push(element->height());
            push(element->pivot().x);
            push(element->pivot().y);
            push(element->anchor().getX());
            push(element->anchor().getY());
            push(element->anchor().getZ());
            push(element->anchor().getW());
            const LayoutChildComponent* layoutChild = enabledLayoutChildOf(element->entity());
            inputs.push_back(layoutChild ? 1u : 0u);
            if (layoutChild) {
                push(layoutChild->minWidth());
                push(layoutChild->minHeight());
                push(layoutChild->maxWidth().value_or(-1.0f));
                push(layoutChild->maxHeight().value_or(-1.0f));
                inputs.push_back((layoutChild->maxWidth() ? 1u : 0u) | (layoutChild->maxHeight() ? 2u : 0u) |
                                 (layoutChild->excludeFromLayout() ? 4u : 0u));
                push(layoutChild->fitWidthProportion());
                push(layoutChild->fitHeightProportion());
            }
        }
    }

    bool LayoutGroupComponent::reflowIfChanged()
    {
        gatherInputs(_currentInputs);
        if (_currentInputs == _lastInputs) {
            return false;
        }
        _lastInputs.swap(_currentInputs);
        reflow();
        return true;
    }

    void LayoutGroupComponent::reflow()
    {
        auto* container = _entity->findComponent<ElementComponent>();
        if (!container) {
            return;
        }

        std::vector<ElementComponent*> elements;
        std::vector<LayoutItem> items;
        for (const auto& child : _entity->children()) {
            ElementComponent* element = enabledElementOf(child.get());
            if (!element) {
                continue;
            }
            const LayoutChildComponent* layoutChild = enabledLayoutChildOf(element->entity());
            if (layoutChild && layoutChild->excludeFromLayout()) {
                continue;
            }
            // The layout child's value when it has one, else the
            // element's (width and height), else the default.
            LayoutItem item;
            item.width = element->width();
            item.height = element->height();
            item.pivot = element->pivot();
            if (layoutChild) {
                item.minWidth = layoutChild->minWidth();
                item.minHeight = layoutChild->minHeight();
                item.maxWidth = layoutChild->maxWidth().value_or(std::numeric_limits<float>::infinity());
                item.maxHeight = layoutChild->maxHeight().value_or(std::numeric_limits<float>::infinity());
                item.fitWidthProportion = layoutChild->fitWidthProportion();
                item.fitHeightProportion = layoutChild->fitHeightProportion();
            }
            elements.push_back(element);
            items.push_back(item);
        }
        if (elements.empty()) {
            return;
        }

        // Anchors other than all-zero make positions hard to reason about, and a split one
        // would make the size follow the group instead; they are forced to zero.
        for (ElementComponent* element : elements) {
            if (element->anchor().getX() != 0.0f || element->anchor().getY() != 0.0f ||
                element->anchor().getZ() != 0.0f || element->anchor().getW() != 0.0f) {
                element->setAnchor(Vector4(0.0f, 0.0f, 0.0f, 0.0f));
            }
        }

        LayoutOptions options = _options;
        options.containerSize = Vector2(std::max(container->calculatedWidth(), 0.0f),
                                        std::max(container->calculatedHeight(), 0.0f));
        const LayoutResult layout = calculateLayout(items, options);

        for (size_t i = 0; i < elements.size(); ++i) {
            ElementComponent* element = elements[i];
            const LayoutPlacement& placement = layout.placements[i];
            element->setCalculatedWidth(placement.width);
            element->setCalculatedHeight(placement.height);
            Entity* entity = element->entity();
            entity->setLocalPosition(placement.x, placement.y, entity->localPosition().getZ());
        }

        // The inputs as the layout left them (it reset the anchors): what the reflow itself
        // changes is ignored, but not what a `reflow` handler changes, such as the group's
        // size, which the next pass then sees differ.
        gatherInputs(_lastInputs);
        fire("reflow", layout.bounds);
    }
}
