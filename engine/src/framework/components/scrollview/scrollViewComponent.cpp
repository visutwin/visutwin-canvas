// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "scrollViewComponent.h"

#include <algorithm>
#include <any>
#include <cmath>
#include <functional>
#include <typeinfo>

#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementDragHelper.h"
#include "framework/components/scrollbar/scrollbarComponent.h"
#include "framework/entity.h"
#include "framework/input/elementInput.h"

namespace visutwin::canvas
{
    namespace
    {
        const ElementInputEvent* inputEventOf(const EventArgs& args)
        {
            if (!args.empty() && args[0].type() == typeid(ElementInputEvent*)) {
                return std::any_cast<ElementInputEvent*>(args[0]);
            }
            return nullptr;
        }

        bool elementStillExists(const ElementComponent* element, const uint64_t serial)
        {
            const auto& all = ElementComponent::instances();
            return std::find(all.begin(), all.end(), element) != all.end() && element->serial() == serial;
        }
    }

    ScrollViewComponent::ScrollViewComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instanceList.add(this);
        setBindingEntity(_self, entity);
    }

    ScrollViewComponent::~ScrollViewComponent()
    {
        _instanceList.remove(this);
        // upstream onBeforeRemove: everything the view refers to may outlive it.
        for (Binding* binding : {&_self, &_viewport, &_content, &_scrollbars[0], &_scrollbars[1]}) {
            unbind(*binding);
            if (binding->destroyed) {
                binding->destroyed->off();
            }
        }
    }

    void ScrollViewComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const ScrollViewComponent*>(source);
        if (!src) {
            return;
        }
        _horizontal = src->_horizontal;
        _vertical = src->_vertical;
        _scrollMode = src->_scrollMode;
        _bounceAmount = src->_bounceAmount;
        _friction = src->_friction;
        _dragThreshold = src->_dragThreshold;
        _useMouseWheel = src->_useMouseWheel;
        _mouseWheelSensitivity = src->_mouseWheelSensitivity;
        _visibility = src->_visibility;
        setViewportEntity(src->_viewport.entity);
        setContentEntity(src->_content.entity);
        setHorizontalScrollbarEntity(src->_scrollbars[0].entity);
        setVerticalScrollbarEntity(src->_scrollbars[1].entity);
    }

    void ScrollViewComponent::resolveClonedReferences(const Component* source, const CloneNodeMap& map)
    {
        const auto* src = dynamic_cast<const ScrollViewComponent*>(source);
        if (!src) {
            return;
        }
        if (src->_viewport.entity) {
            setViewportEntity(remapCloned(src->_viewport.entity, map));
        }
        if (src->_content.entity) {
            setContentEntity(remapCloned(src->_content.entity, map));
        }
        if (src->_scrollbars[0].entity) {
            setHorizontalScrollbarEntity(remapCloned(src->_scrollbars[0].entity, map));
        }
        if (src->_scrollbars[1].entity) {
            setVerticalScrollbarEntity(remapCloned(src->_scrollbars[1].entity, map));
        }
    }

    void ScrollViewComponent::onEnable()
    {
        for (const Binding& binding : _scrollbars) {
            if (binding.scrollbar) {
                binding.scrollbar->setEnabled(true);
            }
        }
        if (_contentDragHelper) {
            _contentDragHelper->setEnabled(true);
        }
        syncAll();
    }

    void ScrollViewComponent::onDisable()
    {
        for (const Binding& binding : _scrollbars) {
            if (binding.scrollbar) {
                binding.scrollbar->setEnabled(false);
            }
        }
        if (_contentDragHelper) {
            _contentDragHelper->setEnabled(false);
        }
    }

    void ScrollViewComponent::setHorizontal(const bool value)
    {
        if (_horizontal != value) {
            _horizontal = value;
            syncScrollbarEnabledState(0);
        }
    }

    void ScrollViewComponent::setVertical(const bool value)
    {
        if (_vertical != value) {
            _vertical = value;
            syncScrollbarEnabledState(1);
        }
    }

    // --- bindings ---------------------------------------------------------------------------

    void ScrollViewComponent::setBindingEntity(Binding& binding, Entity* entity)
    {
        if (binding.entity == entity) {
            return;
        }
        unbind(binding);
        if (binding.destroyed) {
            binding.destroyed->off();
            binding.destroyed.reset();
        }
        binding.entity = entity;
        if (entity && &binding != &_self) {
            binding.destroyed = entity->on("destroy", [this, &binding]() {
                unbind(binding);
                binding.entity = nullptr;
            });
        }
        refreshBindings();
    }

    void ScrollViewComponent::unbind(Binding& binding)
    {
        for (auto& handle : binding.handles) {
            handle->off();
        }
        binding.handles.clear();
        if (&binding == &_content) {
            _contentDragHelper.reset();
        }
        binding.element = nullptr;
        binding.scrollbar = nullptr;
    }

    void ScrollViewComponent::setViewportEntity(Entity* entity) { setBindingEntity(_viewport, entity); }

    void ScrollViewComponent::setContentEntity(Entity* entity) { setBindingEntity(_content, entity); }

    void ScrollViewComponent::setScrollbarEntity(const int axis, Entity* entity)
    {
        setBindingEntity(_scrollbars[axis], entity);
    }

    void ScrollViewComponent::refreshBindings()
    {
        // The view's own element: its size and the mouse wheel over it.
        if (!_self.element && _self.entity) {
            if (auto* element = _self.entity->findComponent<ElementComponent>()) {
                _self.element = element;
                _self.handles.push_back(element->on("resize", [this]() { syncAll(); }));
                _self.handles.push_back(element->on("mousewheel", [this](const EventArgs& args) {
                    onMouseWheel(inputEventOf(args));
                }));
                _self.handles.push_back(element->on("beforeremove", [this]() { unbind(_self); }));
            }
        }

        if (!_viewport.element && _viewport.entity) {
            if (auto* element = _viewport.entity->findComponent<ElementComponent>()) {
                _viewport.element = element;
                _viewport.handles.push_back(element->on("resize", [this]() { syncAll(); }));
                _viewport.handles.push_back(element->on("beforeremove", [this]() { unbind(_viewport); }));
                syncAll();
            }
        }

        if (!_content.element && _content.entity) {
            if (auto* element = _content.entity->findComponent<ElementComponent>()) {
                _content.element = element;
                _content.handles.push_back(element->on("resize", [this]() { syncAll(); }));
                _content.handles.push_back(element->on("beforeremove", [this]() { unbind(_content); }));

                _contentDragHelper = std::make_unique<ElementDragHelper>(element);
                _contentDragHelper->on("drag:start", [this]() { onContentDragStart(); });
                _contentDragHelper->on("drag:end", [this]() { onContentDragEnd(); });
                _contentDragHelper->on("drag:move", [this](const Vector3& position) { onContentDragMove(position); });

                _prevContentSizes = {std::nullopt, std::nullopt};
                syncAll();
            }
        }

        for (int axis = 0; axis < 2; ++axis) {
            Binding& binding = _scrollbars[axis];
            if (binding.scrollbar || !binding.entity) {
                continue;
            }
            if (auto* scrollbar = binding.entity->findComponent<ScrollbarComponent>()) {
                binding.scrollbar = scrollbar;
                binding.handles.push_back(scrollbar->on("set:value", [this, axis](const float value) {
                    if (!_scrollbarUpdateFlags[axis] && active()) {
                        onSetScroll(axis == 0 ? std::optional<float>(value) : std::nullopt,
                                    axis == 1 ? std::optional<float>(value) : std::nullopt, true);
                    }
                }));
                binding.handles.push_back(scrollbar->on("beforeremove", [this, &binding]() { unbind(binding); }));
                syncScrollbarEnabledState(axis);
                syncScrollbarPosition(axis);
            }
        }
    }

    // --- scrolling --------------------------------------------------------------------------

    void ScrollViewComponent::onSetScroll(const std::optional<float> x, const std::optional<float> y,
                                          const bool resetVelocity)
    {
        if (resetVelocity) {
            _velocity = Vector3(0.0f, 0.0f, 0.0f);
        }
        const bool xChanged = updateAxis(x, 0);
        const bool yChanged = updateAxis(y, 1);
        if (xChanged || yChanged) {
            fire("set:scroll", _scroll);
        }
    }

    bool ScrollViewComponent::updateAxis(const std::optional<float> scrollValue, const int axis)
    {
        const bool hasChanged = scrollValue && std::abs(*scrollValue - axisOf(_scroll, axis)) > 1e-5f;
        if (hasChanged || isDragging() || (scrollValue && *scrollValue == 0.0f)) {
            // DEVIATION: an axis the caller did not name keeps its value; upstream clamps its
            // null to 0 (or keeps null in Bounce mode) while the content is dragged.
            const float value = scrollValue.value_or(axisOf(_scroll, axis));
            axisOf(_scroll, axis) = determineNewScrollValue(value, axis);
            syncContentPosition(axis);
            syncScrollbarPosition(axis);
        }
        return hasChanged;
    }

    float ScrollViewComponent::determineNewScrollValue(const float scrollValue, const int axis)
    {
        if (!scrollingEnabled(axis)) {
            return axisOf(_scroll, axis);
        }
        switch (_scrollMode) {
        case ScrollMode::Clamp:
            return std::clamp(scrollValue, 0.0f, maxScrollValue(axis));
        case ScrollMode::Bounce:
            setVelocityFromOvershoot(scrollValue, axis);
            return scrollValue;
        case ScrollMode::Infinite:
            return scrollValue;
        }
        return scrollValue;
    }

    void ScrollViewComponent::syncAll()
    {
        syncContentPosition(0);
        syncContentPosition(1);
        syncScrollbarPosition(0);
        syncScrollbarPosition(1);
        syncScrollbarEnabledState(0);
        syncScrollbarEnabledState(1);
    }

    void ScrollViewComponent::syncContentPosition(const int axis)
    {
        if (!_content.entity) {
            return;
        }
        // Content that changed size keeps its scroll offset in pixels where it can.
        const std::optional<float> prev = _prevContentSizes[axis];
        const float current = contentSize(axis);
        if (prev && std::abs(*prev - current) > 1e-4f) {
            const float prevMaxOffset = maxOffset(axis, *prev);
            const float currMaxOffset = maxOffset(axis, current);
            axisOf(_scroll, axis) = currMaxOffset == 0.0f
                ? 1.0f : std::clamp(axisOf(_scroll, axis) * prevMaxOffset / currMaxOffset, 0.0f, 1.0f);
        }

        const float offset = axisOf(_scroll, axis) * maxOffset(axis);
        const Vector3& local = _content.entity->localPosition();
        _content.entity->setLocalPosition(axis == 0 ? Vector3(offset * sign(axis), local.getY(), local.getZ())
                                                    : Vector3(local.getX(), offset * sign(axis), local.getZ()));
        _prevContentSizes[axis] = current;
    }

    void ScrollViewComponent::syncScrollbarPosition(const int axis)
    {
        ScrollbarComponent* scrollbar = _scrollbars[axis].scrollbar;
        if (!scrollbar) {
            return;
        }
        _scrollbarUpdateFlags[axis] = true;
        scrollbar->setValue(axisOf(_scroll, axis));
        scrollbar->setHandleSize(scrollbarHandleSize(axis));
        _scrollbarUpdateFlags[axis] = false;
    }

    void ScrollViewComponent::syncScrollbarEnabledState(const int axis)
    {
        Entity* entity = _scrollbars[axis].entity;
        if (!entity) {
            return;
        }
        const bool enabled = _visibility[axis] == ScrollbarVisibility::ShowWhenRequired
            ? scrollingEnabled(axis) && contentIsLargerThanViewport(axis) : scrollingEnabled(axis);
        if (entity->enabledLocal() != enabled) {
            entity->setEnabled(enabled);
        }
    }

    Vector2 ScrollViewComponent::contentPositionToScrollValue(const Vector3& position) const
    {
        const float maxOffsetH = maxOffset(0);
        const float maxOffsetV = maxOffset(1);
        return Vector2(maxOffsetH == 0.0f ? 0.0f : position.getX() / maxOffsetH,
                       maxOffsetV == 0.0f ? 0.0f : position.getY() / -maxOffsetV);
    }

    float ScrollViewComponent::maxOffset(const int axis, const std::optional<float> contentSizeOverride) const
    {
        const float content = contentSizeOverride.value_or(contentSize(axis));
        const float viewport = viewportSize(axis);
        if (content < viewport) {
            return -viewport;
        }
        return viewport - content;
    }

    float ScrollViewComponent::scrollbarHandleSize(const int axis) const
    {
        const float viewport = viewportSize(axis);
        const float content = contentSize(axis);
        if (std::abs(content) < 0.001f) {
            return 1.0f;
        }
        const float handleSize = std::min(viewport / content, 1.0f);
        const float overshoot = toOvershoot(axisOf(_scroll, axis), axis);
        return overshoot == 0.0f ? handleSize : handleSize / (1.0f + std::abs(overshoot));
    }

    float ScrollViewComponent::viewportSize(const int axis) const
    {
        const ElementComponent* element = _viewport.element;
        return element ? (axis == 0 ? element->calculatedWidth() : element->calculatedHeight()) : 0.0f;
    }

    float ScrollViewComponent::contentSize(const int axis) const
    {
        const ElementComponent* element = _content.element;
        return element ? (axis == 0 ? element->calculatedWidth() : element->calculatedHeight()) : 0.0f;
    }

    float ScrollViewComponent::toOvershoot(const float scrollValue, const int axis) const
    {
        const float max = maxScrollValue(axis);
        if (scrollValue < 0.0f) {
            return scrollValue;
        }
        if (scrollValue > max) {
            return scrollValue - max;
        }
        return 0.0f;
    }

    bool ScrollViewComponent::isDragging() const { return _contentDragHelper && _contentDragHelper->isDragging(); }

    // --- fling, bounce and drag ------------------------------------------------------------

    void ScrollViewComponent::update()
    {
        if (!_content.entity) {
            return;
        }
        if (!isDragging()) {
            if (_scrollMode == ScrollMode::Bounce) {
                for (int axis = 0; axis < 2; ++axis) {
                    if (hasOvershoot(axis)) {
                        setVelocityFromOvershoot(axisOf(_scroll, axis), axis);
                    }
                }
            }
            if (std::abs(_velocity.getX()) > 1e-4f || std::abs(_velocity.getY()) > 1e-4f) {
                const Vector3& local = _content.entity->localPosition();
                const Vector3 position(local.getX() + _velocity.getX(), local.getY() + _velocity.getY(), local.getZ());
                _content.entity->setLocalPosition(position);
                setScrollFromContentPosition(position);
            }
            // Per frame, as upstream: the fling's decay follows the frame rate.
            _velocity = Vector3(_velocity.getX() * (1.0f - _friction), _velocity.getY() * (1.0f - _friction),
                                _velocity.getZ());
        }
        syncScrollbarEnabledState(0);
        syncScrollbarEnabledState(1);
    }

    void ScrollViewComponent::setVelocityFromOvershoot(const float scrollValue, const int axis)
    {
        const float overshootPixels = toOvershoot(scrollValue, axis) * maxOffset(axis) * sign(axis);
        if (std::abs(overshootPixels) > 0.0f) {
            const float v = -overshootPixels / (_bounceAmount * 50.0f + 1.0f);
            _velocity = axis == 0 ? Vector3(v, _velocity.getY(), _velocity.getZ())
                                  : Vector3(_velocity.getX(), v, _velocity.getZ());
        }
    }

    void ScrollViewComponent::setVelocityFromContentPositionDelta(const Vector3& position)
    {
        if (_prevContentDragPosition) {
            _velocity = position - *_prevContentDragPosition;
            _prevContentDragPosition = position;
        } else {
            _velocity = Vector3(0.0f, 0.0f, 0.0f);
            _prevContentDragPosition = position;
        }
    }

    void ScrollViewComponent::setScrollFromContentPosition(const Vector3& position)
    {
        Vector2 scrollValue = contentPositionToScrollValue(position);
        if (isDragging()) {
            scrollValue = applyScrollValueTension(scrollValue);
        }
        onSetScroll(scrollValue.x, scrollValue.y, false);
    }

    // Past an end the content follows the pointer less and less: log10 of the overshoot.
    Vector2 ScrollViewComponent::applyScrollValueTension(Vector2 scrollValue) const
    {
        for (int axis = 0; axis < 2; ++axis) {
            const float max = maxScrollValue(axis);
            const float overshoot = toOvershoot(axisOf(scrollValue, axis), axis);
            if (overshoot > 0.0f) {
                axisOf(scrollValue, axis) = max + std::log10(1.0f + overshoot);
            } else if (overshoot < 0.0f) {
                axisOf(scrollValue, axis) = -std::log10(1.0f - overshoot);
            }
        }
        return scrollValue;
    }

    void ScrollViewComponent::onContentDragStart()
    {
        if (_content.entity && active()) {
            _dragStartPosition = _content.entity->localPosition();
        }
    }

    void ScrollViewComponent::onContentDragEnd()
    {
        _prevContentDragPosition.reset();
        enableContentInput();
    }

    void ScrollViewComponent::onContentDragMove(const Vector3& position)
    {
        if (!_content.entity || !active()) {
            return;
        }
        setScrollFromContentPosition(position);
        setVelocityFromContentPositionDelta(position);
        if (!_disabledContentInput) {
            const float dx = position.getX() - _dragStartPosition.getX();
            const float dy = position.getY() - _dragStartPosition.getY();
            if (std::abs(dx) > _dragThreshold || std::abs(dy) > _dragThreshold) {
                disableContentInput();
            }
        }
    }

    void ScrollViewComponent::onMouseWheel(const ElementInputEvent* event)
    {
        if (!event || !_useMouseWheel || !_content.element) {
            return;
        }
        const float normalizedDeltaX = event->wheelPixelsX / _content.element->calculatedWidth() *
            _mouseWheelSensitivity.x;
        const float normalizedDeltaY = event->wheelPixelsY / _content.element->calculatedHeight() *
            _mouseWheelSensitivity.y;
        const float scrollX = std::clamp(_scroll.x + normalizedDeltaX, 0.0f, maxScrollValue(0));
        const float scrollY = std::clamp(_scroll.y + normalizedDeltaY, 0.0f, maxScrollValue(1));
        setScroll(Vector2(scrollX, scrollY));
    }

    void ScrollViewComponent::enableContentInput()
    {
        for (const auto& [element, serial] : _disabledContentInputElements) {
            if (elementStillExists(element, serial)) {
                element->setUseInput(true);
            }
        }
        _disabledContentInputElements.clear();
        _disabledContentInput = false;
    }

    // A drag past the threshold is a scroll, not a press: the content's children stop taking
    // input until it ends.
    void ScrollViewComponent::disableContentInput()
    {
        const std::function<void(GraphNode*)> disable = [&](GraphNode* node) {
            if (auto* entity = dynamic_cast<Entity*>(node)) {
                auto* element = entity->findComponent<ElementComponent>();
                if (element && element->useInput()) {
                    _disabledContentInputElements.emplace_back(element, element->serial());
                    element->setUseInput(false);
                }
            }
            for (const auto& child : node->children()) {
                disable(child.get());
            }
        };
        if (_content.entity) {
            for (const auto& child : _content.entity->children()) {
                disable(child.get());
            }
        }
        _disabledContentInput = true;
    }
}
