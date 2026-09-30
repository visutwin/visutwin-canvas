// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A scroll view (upstream framework/components/scroll-view/component.js): a content element,
// larger than its viewport element, that the user drags or scrolls with the mouse wheel,
// with optional scrollbars that follow and drive the position. The viewport is usually a
// mask, so the content shows only inside it.
//
//     view->setViewportEntity(viewport);
//     view->setContentEntity(content);
//     view->setVerticalScrollbarEntity(scrollbar);
//     view->on("set:scroll", [](const Vector2& scroll) { ... });
//
// The scroll position runs 0 to 1 on each axis: from the content's left edge at the
// viewport's left, and from its TOP edge at the viewport's top, to its far edges at the far
// edges of the viewport. Content smaller than the viewport does not scroll. Dragging past
// an end stretches with a log tension and, in Bounce mode, springs back with the friction;
// a drag further than `dragThreshold` turns off input on the content's children until it
// ends, so a drag does not click a button in the list.
//
// DEVIATIONS:
// - upstream binds its elements and scrollbars when `element:add` / `scrollbar:add` say they
//   appeared; those events do not exist here, so the scroll view system looks for them after
//   each update (`refreshBindings`), as the button system does;
// - upstream leaves `horizontal`, `vertical`, `scrollMode`, `bounceAmount` and `friction`
//   undefined until set; here they default to both axes, Bounce, 0.1 and 0.05;
// - the mouse wheel's deltas are the browser's in pixels upstream; here a wheel notch is
//   100 pixels (`ElementInputEvent::wheelPixels`).
//
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "framework/components/component.h"
#include "framework/components/layoutgroup/layoutCalculator.h"

namespace visutwin::canvas
{
    class ElementComponent;
    class ElementDragHelper;
    class Entity;
    class ScrollbarComponent;
    struct ElementInputEvent;

    /// Upstream SCROLL_MODE_*: what happens past an end of the content.
    enum class ScrollMode
    {
        /// Stops at the ends.
        Clamp = 0,
        /// Goes past them while dragged, and springs back.
        Bounce = 1,
        /// No ends.
        Infinite = 2
    };

    /// Upstream SCROLLBAR_VISIBILITY_*.
    enum class ScrollbarVisibility
    {
        ShowAlways = 0,
        /// Only while the content is larger than the viewport on that axis.
        ShowWhenRequired = 1
    };

    class ScrollViewComponent : public Component
    {
    public:
        ScrollViewComponent(IComponentSystem* system, Entity* entity);
        ~ScrollViewComponent() override;

        void initializeComponentData() override { refreshBindings(); }
        void cloneFrom(const Component* source) override;
        void resolveClonedReferences(const Component* source, const CloneNodeMap& map) override;
        void onEnable() override;
        void onDisable() override;

        static const std::vector<ScrollViewComponent*>& instances() { return _instances; }

        bool horizontal() const { return _horizontal; }
        void setHorizontal(bool value);
        bool vertical() const { return _vertical; }
        void setVertical(bool value);
        ScrollMode scrollMode() const { return _scrollMode; }
        void setScrollMode(const ScrollMode value) { _scrollMode = value; }
        /// How far the content bounces back each frame in Bounce mode: 0 at once, larger slower.
        float bounceAmount() const { return _bounceAmount; }
        void setBounceAmount(const float value) { _bounceAmount = value; }
        /// How fast a flicked content slows down, 0 (never) to 1 (at once).
        float friction() const { return _friction; }
        void setFriction(const float value) { _friction = value; }
        /// How far, in the content's units, a drag goes before it turns off the children's input.
        float dragThreshold() const { return _dragThreshold; }
        void setDragThreshold(const float value) { _dragThreshold = value; }
        bool useMouseWheel() const { return _useMouseWheel; }
        void setUseMouseWheel(const bool value) { _useMouseWheel = value; }
        const Vector2& mouseWheelSensitivity() const { return _mouseWheelSensitivity; }
        void setMouseWheelSensitivity(const Vector2& value) { _mouseWheelSensitivity = value; }
        ScrollbarVisibility horizontalScrollbarVisibility() const { return _visibility[0]; }
        void setHorizontalScrollbarVisibility(const ScrollbarVisibility value) { _visibility[0] = value; }
        ScrollbarVisibility verticalScrollbarVisibility() const { return _visibility[1]; }
        void setVerticalScrollbarVisibility(const ScrollbarVisibility value) { _visibility[1] = value; }

        /// The entity whose element shows the content (usually a mask).
        Entity* viewportEntity() const { return _viewport.entity; }
        void setViewportEntity(Entity* entity);
        /// The entity whose element moves: it should be a child of the viewport, anchored to its
        /// top-left, with its pivot there too.
        Entity* contentEntity() const { return _content.entity; }
        void setContentEntity(Entity* entity);
        Entity* horizontalScrollbarEntity() const { return _scrollbars[0].entity; }
        void setHorizontalScrollbarEntity(Entity* entity) { setScrollbarEntity(0, entity); }
        Entity* verticalScrollbarEntity() const { return _scrollbars[1].entity; }
        void setVerticalScrollbarEntity(Entity* entity) { setScrollbarEntity(1, entity); }

        /// 0 to 1 on each axis (past them while bouncing); setting it stops any fling.
        const Vector2& scroll() const { return _scroll; }
        void setScroll(const Vector2& value) { onSetScroll(value.x, value.y, true); }

        /// The fling and bounce, and the scrollbars' visibility (upstream `onUpdate`).
        void update();
        /// Pick up elements and scrollbars added since the last look; the system calls it
        /// after each update, and it costs nothing once everything is bound.
        void refreshBindings();

    private:
        /// An entity the view refers to, the element (or scrollbar) bound on it, and the
        /// subscriptions that go with them.
        struct Binding
        {
            Entity* entity = nullptr;
            EventHandlePtr destroyed;
            ElementComponent* element = nullptr;
            ScrollbarComponent* scrollbar = nullptr;
            std::vector<EventHandlePtr> handles;
        };

        void setBindingEntity(Binding& binding, Entity* entity);
        void unbind(Binding& binding);
        void setScrollbarEntity(int axis, Entity* entity);

        void onSetScroll(std::optional<float> x, std::optional<float> y, bool resetVelocity);
        bool updateAxis(std::optional<float> scrollValue, int axis);
        float determineNewScrollValue(float scrollValue, int axis);
        void syncAll();
        void syncContentPosition(int axis);
        void syncScrollbarPosition(int axis);
        void syncScrollbarEnabledState(int axis);

        void onContentDragStart();
        void onContentDragEnd();
        void onContentDragMove(const Vector3& position);
        void onMouseWheel(const ElementInputEvent* event);
        void setScrollFromContentPosition(const Vector3& position);
        void setVelocityFromContentPositionDelta(const Vector3& position);
        void setVelocityFromOvershoot(float scrollValue, int axis);
        Vector2 contentPositionToScrollValue(const Vector3& position) const;
        Vector2 applyScrollValueTension(Vector2 scrollValue) const;
        void enableContentInput();
        void disableContentInput();

        bool scrollingEnabled(const int axis) const { return axis == 0 ? _horizontal : _vertical; }
        bool isDragging() const;
        float viewportSize(int axis) const;
        float contentSize(int axis) const;
        bool contentIsLargerThanViewport(int axis) const { return contentSize(axis) > viewportSize(axis); }
        float maxOffset(int axis, std::optional<float> contentSizeOverride = std::nullopt) const;
        float maxScrollValue(int axis) const { return contentIsLargerThanViewport(axis) ? 1.0f : 0.0f; }
        float scrollbarHandleSize(int axis) const;
        float toOvershoot(float scrollValue, int axis) const;
        bool hasOvershoot(int axis) const { return std::abs(toOvershoot(axisOf(_scroll, axis), axis)) > 0.001f; }
        static float sign(const int axis) { return axis == 0 ? 1.0f : -1.0f; }
        static float axisOf(const Vector2& v, const int axis) { return axis == 0 ? v.x : v.y; }
        static float& axisOf(Vector2& v, const int axis) { return axis == 0 ? v.x : v.y; }

        static inline std::vector<ScrollViewComponent*> _instances;

        bool _horizontal = true;
        bool _vertical = true;
        ScrollMode _scrollMode = ScrollMode::Bounce;
        float _bounceAmount = 0.1f;
        float _friction = 0.05f;
        float _dragThreshold = 10.0f;
        bool _useMouseWheel = true;
        Vector2 _mouseWheelSensitivity = Vector2(1.0f, 1.0f);
        std::array<ScrollbarVisibility, 2> _visibility = {ScrollbarVisibility::ShowAlways,
                                                          ScrollbarVisibility::ShowAlways};

        Binding _self;
        Binding _viewport;
        Binding _content;
        std::array<Binding, 2> _scrollbars;
        std::array<bool, 2> _scrollbarUpdateFlags = {false, false};
        std::unique_ptr<ElementDragHelper> _contentDragHelper;

        Vector2 _scroll = Vector2(0.0f, 0.0f);
        Vector3 _velocity;
        Vector3 _dragStartPosition;
        std::optional<Vector3> _prevContentDragPosition;
        std::array<std::optional<float>, 2> _prevContentSizes;
        bool _disabledContentInput = false;
        /// Elements whose input a drag turned off, told apart by serial so one destroyed in the
        /// meantime is skipped.
        std::vector<std::pair<ElementComponent*, uint64_t>> _disabledContentInputElements;
    };
}
