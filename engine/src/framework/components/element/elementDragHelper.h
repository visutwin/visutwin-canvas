// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 30.09.2026
//
// Makes an element draggable:
// a mouse press or touch on the element starts a drag, and each move after it sets the
// element entity's local position to where it started plus how far the pointer has moved,
// measured in the plane of the element and in the units of its parent. An axis constrains
// the drag to x or y. The element needs `useInput`, and the pressed element takes every move
// until the release, wherever the pointer goes.
//
//     auto helper = std::make_unique<ElementDragHelper>(element, DragAxis::X);
//     helper->on("drag:move", [](const Vector3& position) { ... });
//
// It fires `drag:start`, `drag:move` (with the new local position) and `drag:end`. The
// helper holds the element by pointer: its owner destroys it before the element goes (the
// element's `beforeremove`), as the scrollbar and the scroll view do. XR select events have
// no counterpart here.
//
#pragma once

#include <vector>

#include "core/eventHandler.h"
#include "core/math/vector3.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class ElementComponent;
    struct ElementInputEvent;

    /// The axis a drag is constrained to: x, y or none.
    enum class DragAxis
    {
        None,
        X,
        Y
    };

    class ElementDragHelper : public EventHandler
    {
    public:
        explicit ElementDragHelper(ElementComponent* element, DragAxis axis = DragAxis::None);
        ~ElementDragHelper();
        ElementDragHelper(const ElementDragHelper&) = delete;
        ElementDragHelper& operator=(const ElementDragHelper&) = delete;

        ElementComponent* element() const { return _element; }
        DragAxis axis() const { return _axis; }

        /// While false, a press starts no drag and moves move nothing.
        bool enabled() const { return _enabled; }
        void setEnabled(const bool value) { _enabled = value; }
        bool isDragging() const { return _isDragging; }

    private:
        void onPress(ElementInputEvent* event);
        void onRelease();
        void onMove(ElementInputEvent* event);
        void toggleDragListeners(bool on);
        /// Where the pointer meets the element's plane, in the element's parent units; false
        /// when it misses.
        bool screenToLocal(const ElementInputEvent& event, Vector3& point) const;
        void calculateDragScale();

        ElementComponent* _element;
        DragAxis _axis;
        bool _enabled = true;
        bool _isDragging = false;
        CameraComponent* _dragCamera = nullptr;
        Vector3 _dragScale;
        Vector3 _dragStartMousePosition;
        Vector3 _dragStartHandlePosition;
        std::vector<EventHandlePtr> _lifecycleHandles;
        std::vector<EventHandlePtr> _dragHandles;
    };
}
