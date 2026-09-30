// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "elementDragHelper.h"

#include <any>
#include <cmath>
#include <typeinfo>

#include "elementComponent.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/entity.h"
#include "framework/input/elementInput.h"

namespace visutwin::canvas
{
    namespace
    {
        ElementInputEvent* inputEventOf(const EventArgs& args)
        {
            if (!args.empty() && args[0].type() == typeid(ElementInputEvent*)) {
                return std::any_cast<ElementInputEvent*>(args[0]);
            }
            return nullptr;
        }

        bool withinScreenSpace(const ElementComponent* element)
        {
            const ScreenComponent* screen = element->screenComponent();
            return screen && screen->screenSpace();
        }
    }

    ElementDragHelper::ElementDragHelper(ElementComponent* element, const DragAxis axis)
        : _element(element), _axis(axis)
    {
        if (!_element) {
            return;
        }
        for (const char* name : {"mousedown", "touchstart"}) {
            _lifecycleHandles.push_back(_element->on(name, [this](const EventArgs& args) {
                if (ElementInputEvent* event = inputEventOf(args)) {
                    onPress(event);
                }
            }));
        }
    }

    ElementDragHelper::~ElementDragHelper()
    {
        for (auto& handle : _lifecycleHandles) {
            handle->off();
        }
        toggleDragListeners(false);
    }

    void ElementDragHelper::toggleDragListeners(const bool on)
    {
        if (!on) {
            for (auto& handle : _dragHandles) {
                handle->off();
            }
            _dragHandles.clear();
            return;
        }
        if (!_dragHandles.empty() || !_element) {
            return;
        }
        // Upstream listens for the mouse events only when there is a mouse and for the touch
        // events only on a touch platform; an event that never arrives costs nothing here.
        for (const char* name : {"mousemove", "touchmove"}) {
            _dragHandles.push_back(_element->on(name, [this](const EventArgs& args) {
                if (ElementInputEvent* event = inputEventOf(args)) {
                    onMove(event);
                }
            }));
        }
        for (const char* name : {"mouseup", "touchend", "touchcancel"}) {
            _dragHandles.push_back(_element->on(name, [this](const EventArgs&) { onRelease(); }));
        }
    }

    void ElementDragHelper::onPress(ElementInputEvent* event)
    {
        if (!_element || _isDragging || !_enabled) {
            return;
        }
        _dragCamera = event->camera;
        calculateDragScale();

        Vector3 current;
        if (screenToLocal(*event, current)) {
            toggleDragListeners(true);
            _isDragging = true;
            _dragStartMousePosition = current;
            _dragStartHandlePosition = _element->entity()->localPosition();
            fire("drag:start");
        }
    }

    void ElementDragHelper::onRelease()
    {
        if (_isDragging) {
            _isDragging = false;
            toggleDragListeners(false);
            fire("drag:end");
        }
    }

    void ElementDragHelper::onMove(ElementInputEvent* event)
    {
        if (!_element || !_isDragging || !_enabled || !_element->enabled() || !_element->entity()->enabled()) {
            return;
        }
        Vector3 current;
        if (!screenToLocal(*event, current)) {
            return;
        }
        Entity* entity = _element->entity();
        const Vector3 delta = current - _dragStartMousePosition;
        Vector3 position = _dragStartHandlePosition + delta;
        if (_axis != DragAxis::None) {
            // The other axis stays where it is.
            const Vector3& now = entity->localPosition();
            position = _axis == DragAxis::X ? Vector3(position.getX(), now.getY(), position.getZ())
                                            : Vector3(now.getX(), position.getY(), position.getZ());
        }
        entity->setLocalPosition(position);
        fire("drag:move", position);
    }

    bool ElementDragHelper::screenToLocal(const ElementInputEvent& event, Vector3& point) const
    {
        // upstream _chooseRayOriginAndDirection: a screen-space element is hit by a ray straight
        // into the screen from the pointer, anything else by a ray from the camera through it.
        // Only differences between two points are used, so the screen-space origin need not be
        // in the element's space.
        Vector3 origin;
        Vector3 direction;
        if (withinScreenSpace(_element)) {
            origin = Vector3(event.x, -event.y, 0.0f);
            direction = Vector3(0.0f, 0.0f, -1.0f);
        } else {
            if (!_dragCamera || !_dragCamera->entity()) {
                return false;
            }
            const Vector3 world = _dragCamera->screenToWorld(event.x, event.y, 1.0f);
            origin = _dragCamera->entity()->position();
            direction = (world - origin).normalized();
        }

        // The plane of the element: through its world position, facing back along its forward.
        Entity* entity = _element->entity();
        const Vector4 zAxis = entity->worldTransform().getColumn(2);
        const Vector3 normal = Vector3(zAxis.getX(), zAxis.getY(), zAxis.getZ()).normalized();
        const Vector3 onPlane = entity->position();
        const float denominator = normal.dot(direction);
        if (denominator == 0.0f) {
            return false;
        }
        const float t = normal.dot(onPlane - origin) / denominator;
        if (t < 0.0f) {
            return false;
        }
        const Vector3 hit = origin + direction * t;

        // Into the element's rotation, and into its parent's units.
        const Vector3 local = entity->rotation().invert() * hit;
        point = Vector3(local.getX() * _dragScale.getX(), local.getY() * _dragScale.getY(),
                        local.getZ() * _dragScale.getZ());
        return true;
    }

    void ElementDragHelper::calculateDragScale()
    {
        const bool screenSpace = withinScreenSpace(_element);
        const ScreenComponent* screen = _element->screenComponent();
        const float screenScale = screenSpace && screen ? screen->scale() : 1.0f;
        float sx = screenScale;
        float sy = screenScale;

        // The parent scales down to the screen (a screen-space element) or to the root. The
        // element may be the screen's direct child, so each ancestor is tested before its scale
        // is applied.
        for (GraphNode* current = _element->entity()->parent(); current; current = current->parent()) {
            if (screenSpace) {
                auto* entity = dynamic_cast<Entity*>(current);
                if (entity && entity->findComponent<ScreenComponent>()) {
                    break;
                }
            }
            sx *= current->localScale().getX();
            sy *= current->localScale().getY();
        }
        _dragScale = Vector3(1.0f / sx, 1.0f / sy, 0.0f);
    }
}
