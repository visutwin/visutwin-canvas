// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// ElementInput's input half: upstream element-input.js from the event handlers down to
// the hit test. The drawing half is elementInput.cpp.
//
#include "elementInput.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

#include <SDL3/SDL.h>

#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "framework/components/button/buttonComponent.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/element/elementComponent.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/camera.h"
#include "scene/composition/layerComposition.h"
#include "scene/constants.h"
#include "scene/scene.h"

namespace visutwin::canvas
{
    namespace
    {
        /// Upstream's click brake: a mouse click within this long of a touch click on the
        /// same element is the platform's synthesized copy of it.
        constexpr auto kClickBrake = std::chrono::milliseconds(300);

        struct Segment
        {
            Vector3 p;
            Vector3 q;
        };

        float scalarTriple(const Vector3& a, const Vector3& b, const Vector3& c) { return a.cross(b).dot(c); }

        /// Upstream `intersectLineQuad` (Real-Time Collision Detection): the squared distance
        /// from p to where line pq crosses the quad with counter-clockwise `corners`, or -1.
        float intersectLineQuad(const Vector3& p, const Vector3& q, const std::array<Vector3, 4>& corners)
        {
            const Vector3 pq = q - p;
            const Vector3 pa = corners[0] - p;
            const Vector3 pb = corners[1] - p;
            const Vector3 pc = corners[2] - p;

            // Which triangle: test against the diagonal first.
            const Vector3 m = pc.cross(pq);
            float v = pa.dot(m);
            float u = 0.0f;
            float w = 0.0f;
            Vector3 hit;
            if (v >= 0.0f) {
                // Triangle abc.
                u = -pb.dot(m);
                if (u < 0.0f) {
                    return -1.0f;
                }
                w = scalarTriple(pq, pb, pa);
                if (w < 0.0f) {
                    return -1.0f;
                }
                const float denom = 1.0f / (u + v + w);
                hit = corners[0] * (u * denom) + corners[1] * (v * denom) + corners[2] * (w * denom);
            } else {
                // Triangle dac.
                const Vector3 pd = corners[3] - p;
                u = pd.dot(m);
                if (u < 0.0f) {
                    return -1.0f;
                }
                w = scalarTriple(pq, pa, pd);
                if (w < 0.0f) {
                    return -1.0f;
                }
                v = -v;
                const float denom = 1.0f / (u + v + w);
                hit = corners[0] * (u * denom) + corners[3] * (v * denom) + corners[2] * (w * denom);
            }

            // The test above passes a degenerate quad; a rectangle with a zero diagonal is one.
            constexpr float kEps = 0.0001f * 0.0001f;
            if ((corners[0] - corners[2]).lengthSquared() < kEps || (corners[1] - corners[3]).lengthSquared() < kEps) {
                return -1.0f;
            }
            return (hit - p).lengthSquared();
        }

        /// The layers an element's visual is drawn on, as ElementInput's drawing picks them.
        std::vector<int> elementLayers(const ElementComponent* element)
        {
            if (!element->layers().empty()) {
                return element->layers();
            }
            return {element->screen() ? LAYERID_UI : LAYERID_WORLD};
        }

        Entity* parentEntity(const Entity* entity)
        {
            return entity ? dynamic_cast<Entity*>(entity->parent()) : nullptr;
        }

        /// Upstream `calculateScaleToScreen`: the screen's scale times the local scales up to
        /// the screen entity.
        Vector3 scaleToScreen(const ElementComponent* element)
        {
            const float s = element->screenComponent() ? element->screenComponent()->scale() : 1.0f;
            Vector3 scale(s, s, s);
            for (Entity* current = element->entity(); current && !current->findComponent<ScreenComponent>();
                 current = parentEntity(current)) {
                scale = scale * current->localScale();
            }
            return scale;
        }

        /// Upstream `calculateScaleToWorld`: every local scale up to the root.
        Vector3 scaleToWorld(const ElementComponent* element)
        {
            Vector3 scale(1.0f, 1.0f, 1.0f);
            for (Entity* current = element->entity(); current; current = parentEntity(current)) {
                scale = scale * current->localScale();
            }
            return scale;
        }

        float checkElement(const Segment& ray, ElementComponent* element, const bool screen)
        {
            const Vector3 scale = screen ? scaleToScreen(element) : scaleToWorld(element);
            const auto corners = ElementInput::buildHitCorners(element, screen ? element->screenCorners()
                                                                               : element->worldCorners(), scale);
            return intersectLineQuad(ray.p, ray.q, corners);
        }
    }

    ElementInput::~ElementInput()
    {
        detach();
    }

    std::array<Vector3, 4> ElementInput::buildHitCorners(ElementComponent* element, const std::array<Vector3, 4>& corners,
                                                         const Vector3& scale)
    {
        std::array<Vector3, 4> hit = corners;
        const ButtonComponent* button = element && element->entity()
            ? element->entity()->findComponent<ButtonComponent>() : nullptr;
        if (button) {
            // Padding goes along the entity's own up and right, so a rotated button grows
            // along its sides (left, bottom, right, top).
            const Matrix4& world = element->entity()->worldTransform();
            const Vector3 up = Vector3(world.getColumn(1)).normalized();
            const Vector3 right = Vector3(world.getColumn(0)).normalized();
            const Vector4& padding = button->hitPadding();
            const Vector3 top = up * (padding.getW() * scale.getY());
            const Vector3 bottom = up * (-padding.getY() * scale.getY());
            const Vector3 rightPad = right * (padding.getZ() * scale.getX());
            const Vector3 leftPad = right * (-padding.getX() * scale.getX());
            hit[0] = corners[0] + bottom + leftPad;
            hit[1] = corners[1] + bottom + rightPad;
            hit[2] = corners[2] + top + rightPad;
            hit[3] = corners[3] + top + leftPad;
        }

        // Keep the order bottom left, bottom right, top right, top left under a negative
        // scale: swap what counts as left and right, or bottom and top.
        if (scale.getX() < 0.0f) {
            const float left = hit[2].getX();
            const float right = hit[0].getX();
            hit[0] = Vector3(left, hit[0].getY(), hit[0].getZ());
            hit[1] = Vector3(right, hit[1].getY(), hit[1].getZ());
            hit[2] = Vector3(right, hit[2].getY(), hit[2].getZ());
            hit[3] = Vector3(left, hit[3].getY(), hit[3].getZ());
        }
        if (scale.getY() < 0.0f) {
            const float bottom = hit[2].getY();
            const float top = hit[0].getY();
            hit[0] = Vector3(hit[0].getX(), bottom, hit[0].getZ());
            hit[1] = Vector3(hit[1].getX(), bottom, hit[1].getZ());
            hit[2] = Vector3(hit[2].getX(), top, hit[2].getZ());
            hit[3] = Vector3(hit[3].getX(), top, hit[3].getZ());
        }
        // A negative z turns the whole element over: swap corners 0 and 2.
        if (scale.getZ() < 0.0f) {
            std::swap(hit[0], hit[2]);
        }
        return hit;
    }

    // ---- targeting ------------------------------------------------------------------------

    std::vector<CameraComponent*> ElementInput::sortedCameras() const
    {
        std::vector<CameraComponent*> cameras;
        for (auto* camera : CameraComponent::instances()) {
            if (camera && camera->camera() && camera->entity() && camera->entity()->engine() == _engine.get() &&
                camera->active()) {
                cameras.push_back(camera);
            }
        }
        // Upstream's camera list is in priority order, the order the cameras draw in.
        std::stable_sort(cameras.begin(), cameras.end(), [](const CameraComponent* a, const CameraComponent* b) {
            return a->priority() < b->priority();
        });
        return cameras;
    }

    ElementComponent* ElementInput::elementAt(CameraComponent* camera, const float x, const float y)
    {
        if (!_engine || !camera || !camera->camera() || !camera->entity()) {
            return nullptr;
        }
        const auto [cw, ch] = _engine->canvasSize();
        if (cw <= 0 || ch <= 0) {
            return nullptr;
        }
        const float sw = static_cast<float>(cw);
        const float sh = static_cast<float>(ch);

        // The camera's rectangle on the canvas (its y is from the bottom).
        const Vector4& rect = camera->camera()->rect();
        const float cameraWidth = rect.getZ() * sw;
        const float cameraHeight = rect.getW() * sh;
        const float cameraLeft = rect.getX() * sw;
        const float cameraRight = cameraLeft + cameraWidth;
        const float cameraBottom = (1.0f - rect.getY()) * sh;
        const float cameraTop = cameraBottom - cameraHeight;
        if (x < cameraLeft || x > cameraRight || y > cameraBottom || y < cameraTop || cameraWidth <= 0.0f ||
            cameraHeight <= 0.0f) {
            return nullptr;
        }
        const float localX = sw * (x - cameraLeft) / cameraWidth;
        const float localY = sh * (y - cameraTop) / cameraHeight;

        // A screen-space element is hit by a ray straight into the screen, whose corners are
        // canvas points with y up from the bottom.
        const Segment rayScreen{Vector3(localX, sh - localY, 1.0f), Vector3(localX, sh - localY, -1.0f)};

        // Any other by a ray from the near plane to the far plane through the point.
        std::optional<Segment> ray3d;
        {
            const float ndcX = localX / sw * 2.0f - 1.0f;
            const float ndcY = 1.0f - localY / sh * 2.0f;
            const Matrix4 view = camera->entity()->worldTransform().inverse();
            const Matrix4 inverseViewProjection = (camera->camera()->projectionMatrix() * view).inverse();
            const Vector4 nearClip = inverseViewProjection * Vector4(ndcX, ndcY, -1.0f, 1.0f);
            const Vector4 farClip = inverseViewProjection * Vector4(ndcX, ndcY, 1.0f, 1.0f);
            if (std::abs(nearClip.getW()) > 1e-8f && std::abs(farClip.getW()) > 1e-8f) {
                ray3d = Segment{nearClip.perspectiveDivide(), farClip.perspectiveDivide()};
            }
        }

        // The candidates, front first (upstream `_sortElements`).
        std::vector<ElementComponent*> elements;
        for (auto* element : ElementComponent::instances()) {
            if (element && element->useInput() && element->entity() && element->entity()->engine() == _engine.get() &&
                element->active()) {
                elements.push_back(element);
            }
        }
        const LayerComposition* composition = _engine->scene() ? _engine->scene()->layers().get() : nullptr;
        const auto compare = [composition](const ElementComponent* a, const ElementComponent* b) {
            if (composition) {
                if (const int order = composition->sortTransparentLayers(elementLayers(a), elementLayers(b)); order != 0) {
                    return order;
                }
            }
            const ScreenComponent* sa = a->screen() ? a->screenComponent() : nullptr;
            const ScreenComponent* sb = b->screen() ? b->screenComponent() : nullptr;
            if (sa && !sb) {
                return -1;
            }
            if (!sa && sb) {
                return 1;
            }
            if (!sa && !sb) {
                return 0;
            }
            if (sa->screenSpace() && !sb->screenSpace()) {
                return -1;
            }
            if (sb->screenSpace() && !sa->screenSpace()) {
                return 1;
            }
            return b->drawOrder() - a->drawOrder();
        };
        std::stable_sort(elements.begin(), elements.end(),
            [&compare](const ElementComponent* a, const ElementComponent* b) { return compare(a, b) < 0; });

        ElementComponent* result = nullptr;
        float closest = std::numeric_limits<float>::infinity();
        for (auto* element : elements) {
            // Only an element on a layer this camera draws.
            const auto layers = elementLayers(element);
            if (std::none_of(layers.begin(), layers.end(), [camera](const int id) { return camera->rendersLayer(id); })) {
                continue;
            }
            const ScreenComponent* screen = element->screen() ? element->screenComponent() : nullptr;
            if (screen && screen->screenSpace()) {
                // A screen-space element that is hit wins outright.
                if (checkElement(rayScreen, element, true) >= 0.0f) {
                    result = element;
                    break;
                }
            } else if (ray3d) {
                const float distance = checkElement(*ray3d, element, false);
                if (distance >= 0.0f) {
                    if (distance < closest) {
                        result = element;
                        closest = distance;
                    }
                    // An element on a (world-space) screen takes precedence.
                    if (screen) {
                        result = element;
                        break;
                    }
                }
            }
        }
        return result;
    }

    std::pair<ElementComponent*, CameraComponent*> ElementInput::targetAt(const float x, const float y)
    {
        // Cameras from the last drawn back, so what is drawn on top is hit first.
        const auto cameras = sortedCameras();
        CameraComponent* camera = nullptr;
        for (auto it = cameras.rbegin(); it != cameras.rend(); ++it) {
            camera = *it;
            if (auto* element = elementAt(camera, x, y)) {
                return {element, camera};
            }
        }
        return {nullptr, camera};
    }

    // ---- delivery -------------------------------------------------------------------------

    void ElementInput::fireEvent(const char* name, ElementInputEvent& event)
    {
        ElementInputEvent* pointer = &event;
        for (ElementComponent* element = event.element; element;) {
            element->fire(name, pointer);
            if (event.propagationStopped()) {
                _propagationStopped = true;
                break;
            }
            Entity* parent = parentEntity(element->entity());
            element = parent ? parent->findComponent<ElementComponent>() : nullptr;
        }
    }

    void ElementInput::watchElement(ElementComponent* element)
    {
        if (!element || _watched.contains(element)) {
            return;
        }
        _watched[element] = element->on("beforeremove", [this, element]() { forgetElement(element); });
    }

    void ElementInput::forgetElement(ElementComponent* element)
    {
        if (_hoveredElement == element) {
            _hoveredElement = nullptr;
        }
        if (_pressedElement == element) {
            _pressedElement = nullptr;
        }
        std::erase_if(_touchedElements, [element](const auto& entry) { return entry.second.element == element; });
        _clickedElements.erase(element);
        // The handle is dropped, not switched off: this runs inside its own dispatch, and
        // the element that owns it is about to go.
        _watched.erase(element);
    }

    void ElementInput::onElementMouseEvent(const char* eventType, const float x, const float y, const MouseButton button,
                                           const int wheelDelta, const KeyModifiers& modifiers)
    {
        const auto [element, camera] = targetAt(x, y);
        ElementComponent* lastHovered = _hoveredElement;
        _hoveredElement = element;
        watchElement(element);

        const auto makeEvent = [&](ElementComponent* target) {
            ElementInputEvent event;
            event.element = target;
            event.camera = camera;
            event.x = x;
            event.y = y;
            event.dx = x - _lastX;
            event.dy = y - _lastY;
            event.button = button;
            event.wheelDelta = wheelDelta;
            event.modifiers = modifiers;
            return event;
        };

        const std::string type = eventType;
        // A pressed element takes every move and the release, wherever they happen.
        if ((type == "mousemove" || type == "mouseup") && _pressedElement) {
            auto event = makeEvent(_pressedElement);
            fireEvent(eventType, event);
        } else if (element) {
            auto event = makeEvent(element);
            fireEvent(eventType, event);
            if (type == "mousedown") {
                _pressedElement = element;
            }
        }

        if (lastHovered != _hoveredElement) {
            if (lastHovered) {
                auto event = makeEvent(lastHovered);
                fireEvent("mouseleave", event);
            }
            if (_hoveredElement) {
                auto event = makeEvent(_hoveredElement);
                fireEvent("mouseenter", event);
            }
        }

        if (type == "mouseup" && _pressedElement) {
            if (_pressedElement == _hoveredElement) {
                // Unless a touch has just clicked it, which the platform echoes as a mouse click.
                bool fireClick = true;
                if (const auto it = _clickedElements.find(_hoveredElement); it != _clickedElements.end()) {
                    fireClick = std::chrono::steady_clock::now() - it->second > kClickBrake;
                    _clickedElements.erase(it);
                }
                if (fireClick) {
                    auto event = makeEvent(_hoveredElement);
                    fireEvent("click", event);
                }
            }
            _pressedElement = nullptr;
        }
    }

    void ElementInput::onMouseDown(const float x, const float y, const MouseButton button, const KeyModifiers& modifiers)
    {
        if (!_enabled || (_engine && _engine->mouse() && _engine->mouse()->relativeMode())) {
            return;
        }
        onElementMouseEvent("mousedown", x, y, button, 0, modifiers);
    }

    void ElementInput::onMouseUp(const float x, const float y, const MouseButton button, const KeyModifiers& modifiers)
    {
        if (!_enabled || (_engine && _engine->mouse() && _engine->mouse()->relativeMode())) {
            return;
        }
        onElementMouseEvent("mouseup", x, y, button, 0, modifiers);
    }

    void ElementInput::onMouseMove(const float x, const float y, const KeyModifiers& modifiers)
    {
        if (!_enabled) {
            return;
        }
        onElementMouseEvent("mousemove", x, y, MouseButton::None, 0, modifiers);
        _lastX = x;
        _lastY = y;
    }

    void ElementInput::onMouseWheel(const float x, const float y, const float deltaY, const KeyModifiers& modifiers)
    {
        if (!_enabled) {
            return;
        }
        // Upstream snaps the browser's deltaY, positive TOWARD the user, to its sign.
        const int wheelDelta = deltaY > 0.0f ? -1 : (deltaY < 0.0f ? 1 : 0);
        onElementMouseEvent("mousewheel", x, y, MouseButton::None, wheelDelta, modifiers);
    }

    void ElementInput::onTouchStart(const int64_t id, const float x, const float y)
    {
        if (!_enabled) {
            return;
        }
        const auto [element, camera] = targetAt(x, y);
        if (!element) {
            return;
        }
        watchElement(element);
        const auto old = _touchedElements.find(id);
        if (old == _touchedElements.end() || old->second.element != element) {
            ElementInputEvent event;
            event.element = element;
            event.camera = camera;
            event.x = x;
            event.y = y;
            event.touch = true;
            event.touchId = id;
            fireEvent("touchstart", event);
            _touchLeaveFired[id] = false;
        }
        _touchedElements[id] = TouchInfo{element, camera, x, y};
    }

    void ElementInput::onTouchMove(const int64_t id, const float x, const float y)
    {
        if (!_enabled) {
            return;
        }
        const auto old = _touchedElements.find(id);
        if (old == _touchedElements.end()) {
            return;
        }
        const TouchInfo info = old->second;
        const auto [element, camera] = targetAt(x, y);
        const auto makeEvent = [&] {
            ElementInputEvent event;
            event.element = info.element;
            event.camera = info.camera;
            event.x = x;
            event.y = y;
            event.touch = true;
            event.touchId = id;
            return event;
        };
        // Leaving the element fires touchleave once for the rest of this touch; the moves
        // keep going to the element it started on.
        if (element != info.element && !_touchLeaveFired[id]) {
            auto event = makeEvent();
            fireEvent("touchleave", event);
            _touchLeaveFired[id] = true;
        }
        if (_touchedElements.contains(id)) {
            auto event = makeEvent();
            fireEvent("touchmove", event);
        }
    }

    void ElementInput::onTouchEnd(const int64_t id, const float x, const float y)
    {
        if (!_enabled) {
            return;
        }
        // Upstream clears the brake of every earlier touch here.
        _clickedElements.clear();
        const auto it = _touchedElements.find(id);
        if (it == _touchedElements.end()) {
            return;
        }
        const TouchInfo info = it->second;
        _touchedElements.erase(it);
        _touchLeaveFired.erase(id);

        const auto makeEvent = [&] {
            ElementInputEvent event;
            event.element = info.element;
            event.camera = info.camera;
            event.x = info.x;
            event.y = info.y;
            event.touch = true;
            event.touchId = id;
            return event;
        };
        // Released over the element it started on: a click.
        const auto cameras = sortedCameras();
        for (auto camera = cameras.rbegin(); camera != cameras.rend(); ++camera) {
            if (elementAt(*camera, x, y) == info.element && !_clickedElements.contains(info.element)) {
                auto event = makeEvent();
                fireEvent("click", event);
                _clickedElements[info.element] = std::chrono::steady_clock::now();
            }
        }
        auto event = makeEvent();
        fireEvent("touchend", event);
    }

    void ElementInput::onTouchCancel(const int64_t id, const float x, const float y)
    {
        if (!_enabled) {
            return;
        }
        // DEVIATION: upstream routes touchcancel through its touchend handler, which fires a
        // click when the finger is still over the element and names the event after the DOM
        // event's type. A cancelled touch is not a click here; it fires `touchcancel` alone.
        const auto it = _touchedElements.find(id);
        if (it == _touchedElements.end()) {
            return;
        }
        const TouchInfo info = it->second;
        _touchedElements.erase(it);
        _touchLeaveFired.erase(id);
        ElementInputEvent event;
        event.element = info.element;
        event.camera = info.camera;
        event.x = x;
        event.y = y;
        event.touch = true;
        event.touchId = id;
        fireEvent("touchcancel", event);
    }

    // ---- SDL -------------------------------------------------------------------------------

    namespace
    {
        MouseButton buttonOf(const Uint8 sdlButton)
        {
            switch (sdlButton) {
            case SDL_BUTTON_LEFT:   return MouseButton::Left;
            case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
            case SDL_BUTTON_RIGHT:  return MouseButton::Right;
            default:                return MouseButton::None;
            }
        }

        KeyModifiers currentModifiers()
        {
            const SDL_Keymod mod = SDL_GetModState();
            return KeyModifiers{
                .shift = (mod & SDL_KMOD_SHIFT) != 0,
                .control = (mod & SDL_KMOD_CTRL) != 0,
                .alt = (mod & SDL_KMOD_ALT) != 0,
                .meta = (mod & SDL_KMOD_GUI) != 0,
            };
        }

        bool isDirectTouch(const SDL_TouchFingerEvent& finger)
        {
            // SDL's touch device list exists only once video is up; asked before that,
            // SDL_GetTouchDeviceType dereferences it and crashes.
            return SDL_WasInit(SDL_INIT_VIDEO) != 0 &&
                SDL_GetTouchDeviceType(finger.touchID) == SDL_TOUCH_DEVICE_DIRECT;
        }
    }

    bool ElementInput::handleEvent(const SDL_Event& event)
    {
        if (!_engine) {
            return false;
        }
        _propagationStopped = false;
        switch (event.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.which != SDL_TOUCH_MOUSEID) {
                onMouseDown(event.button.x, event.button.y, buttonOf(event.button.button), currentModifiers());
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.which != SDL_TOUCH_MOUSEID) {
                onMouseUp(event.button.x, event.button.y, buttonOf(event.button.button), currentModifiers());
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.which != SDL_TOUCH_MOUSEID) {
                onMouseMove(event.motion.x, event.motion.y, currentModifiers());
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            if (event.wheel.which == SDL_TOUCH_MOUSEID) {
                break;
            }
            const float deltaY = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
            onMouseWheel(event.wheel.mouse_x, event.wheel.mouse_y, deltaY, currentModifiers());
            break;
        }
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            if (!isDirectTouch(event.tfinger)) {
                break;
            }
            // SDL's finger positions are fractions of the window.
            const auto [w, h] = _engine->canvasSize();
            const float x = event.tfinger.x * static_cast<float>(w);
            const float y = event.tfinger.y * static_cast<float>(h);
            const auto id = static_cast<int64_t>(event.tfinger.fingerID);
            if (event.type == SDL_EVENT_FINGER_DOWN) {
                onTouchStart(id, x, y);
            } else if (event.type == SDL_EVENT_FINGER_MOTION) {
                onTouchMove(id, x, y);
            } else if (event.type == SDL_EVENT_FINGER_UP) {
                onTouchEnd(id, x, y);
            } else {
                onTouchCancel(id, x, y);
            }
            break;
        }
        default:
            break;
        }
        return _propagationStopped;
    }
}
