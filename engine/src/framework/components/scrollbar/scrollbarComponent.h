// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A scrollbar (upstream framework/components/scrollbar/component.js): a track, its entity's
// element, and a handle, the element of `handleEntity`, that the scrollbar sizes to
// `handleSize` of the track and places at `value` along it. Dragging the handle sets the
// value, which it fires as `set:value`; with a handle smaller than the track it is a slider.
//
//     scrollbar->on("set:value", [](float value) { ... });
//
// The value runs 0 to 1 from the left of a horizontal track, and from the TOP of a vertical
// one. The handle is placed by its local position along the axis, so it should be anchored
// to the track's left (horizontal) or top (vertical) edge.
//
// DEVIATION: upstream binds the elements when an `element:add` event says one appeared; that
// event does not exist here, so the scrollbar system looks for them after each update
// (`refreshBindings`), as the button system does.
//
#pragma once

#include <memory>
#include <vector>

#include "framework/components/component.h"
#include "framework/components/layoutgroup/layoutCalculator.h"
#include "framework/components/componentInstanceList.h"

namespace visutwin::canvas
{
    class ElementComponent;
    class ElementDragHelper;
    class Entity;

    class ScrollbarComponent : public Component
    {
    public:
        ScrollbarComponent(IComponentSystem* system, Entity* entity);
        ~ScrollbarComponent() override;

        void initializeComponentData() override { refreshBindings(); }
        void cloneFrom(const Component* source) override;
        void resolveClonedReferences(const Component* source, const CloneNodeMap& map) override;
        void onEnable() override;
        void onDisable() override;

        static const std::vector<ScrollbarComponent*>& instances() { return _instanceList.items(); }

        Orientation orientation() const { return _orientation; }
        void setOrientation(Orientation value);
        /// 0 to 1; fires `set:value` when it changes.
        float value() const { return _value; }
        void setValue(float value);
        /// The handle's length as a fraction of the track's, 0 to 1.
        float handleSize() const { return _handleSize; }
        void setHandleSize(float value);
        /// The entity whose element is the handle; dropped (reads null) when it is destroyed.
        Entity* handleEntity() const { return _handleEntity; }
        void setHandleEntity(Entity* entity);

        /// Pick up the track or handle element added since the last look; the scrollbar
        /// system calls it after each update, and it costs nothing once both are bound.
        void refreshBindings();

    private:
        void bindTrackElement(ElementComponent* element);
        void unbindTrackElement();
        void bindHandleElement(ElementComponent* element);
        void unbindHandleElement();
        void rebuildDragHelper();
        void onHandleDrag(float position);
        void updateHandlePositionAndSize();

        float trackLength() const;
        float handleLength() const { return trackLength() * _handleSize; }
        float usableTrackLength() const;
        float sign() const { return _orientation == Orientation::Horizontal ? 1.0f : -1.0f; }

        inline static ComponentInstanceList<ScrollbarComponent> _instanceList;
        Orientation _orientation = Orientation::Horizontal;
        float _value = 0.0f;
        float _handleSize = 0.0f;
        Entity* _handleEntity = nullptr;
        EventHandlePtr _handleEntityDestroyed;
        ElementComponent* _trackElement = nullptr;
        std::vector<EventHandlePtr> _trackHandles;
        ElementComponent* _handleElement = nullptr;
        std::vector<EventHandlePtr> _handleHandles;
        std::unique_ptr<ElementDragHelper> _dragHelper;
    };
}
