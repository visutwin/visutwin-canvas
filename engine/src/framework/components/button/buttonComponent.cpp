// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "buttonComponent.h"

#include <algorithm>
#include <any>
#include <cmath>
#include <cstring>
#include <typeinfo>

#include "framework/components/element/elementComponent.h"
#include "framework/entity.h"
#include "framework/input/elementInput.h"

namespace visutwin::canvas
{
    namespace
    {
        /// The element events a button follows (upstream `_toggleHitElementListeners`; the
        /// XR select events have no counterpart here).
        constexpr const char* kHitEvents[] = {"mouseenter", "mouseleave", "mousedown", "mouseup", "touchstart",
                                              "touchend", "touchleave", "touchcancel", "click"};

        ElementInputEvent* inputEventOf(const EventArgs& args)
        {
            if (!args.empty() && args[0].type() == typeid(ElementInputEvent*)) {
                return std::any_cast<ElementInputEvent*>(args[0]);
            }
            return nullptr;
        }

        bool sameRgb(const Color& a, const Color& b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
    }

    ButtonComponent::ButtonComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instanceList.add(this);
    }

    ButtonComponent::~ButtonComponent()
    {
        _instanceList.remove(this);
        // The elements and the image entity may outlive the button; their events must not
        // call back into a freed component.
        unbindHitElement();
        for (auto& handle : _imageHandles) {
            handle->off();
        }
        _imageHandles.clear();
        if (_imageEntityDestroyed) {
            _imageEntityDestroyed->off();
        }
    }

    void ButtonComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const ButtonComponent*>(source);
        if (!src) {
            return;
        }
        _active = src->_active;
        _hitPadding = src->_hitPadding;
        _transitionMode = src->_transitionMode;
        _hoverTint = src->_hoverTint;
        _pressedTint = src->_pressedTint;
        _inactiveTint = src->_inactiveTint;
        _fadeDuration = src->_fadeDuration;
        _hoverSprite = src->_hoverSprite;
        _hoverSpriteFrame = src->_hoverSpriteFrame;
        _pressedSprite = src->_pressedSprite;
        _pressedSpriteFrame = src->_pressedSpriteFrame;
        _inactiveSprite = src->_inactiveSprite;
        _inactiveSpriteFrame = src->_inactiveSpriteFrame;
        setImageEntity(src->_imageEntity);
    }

    void ButtonComponent::resolveClonedReferences(const Component* source, const CloneNodeMap& map)
    {
        if (const auto* src = dynamic_cast<const ButtonComponent*>(source)) {
            setImageEntity(remapCloned(src->_imageEntity, map));
        }
    }

    // ---- properties ------------------------------------------------------------------------

    void ButtonComponent::setActive(const bool value)
    {
        if (_active == value) {
            return;
        }
        _active = value;
        updateVisualState();
    }

    void ButtonComponent::setImageEntity(Entity* entity)
    {
        if (_imageEntity == entity) {
            return;
        }
        if (_imageEntityDestroyed) {
            _imageEntityDestroyed->off();
            _imageEntityDestroyed.reset();
        }
        if (_imageElement) {
            unbindImageElement(true);
        }
        _imageEntity = entity;
        if (_imageEntity) {
            _imageEntityDestroyed = _imageEntity->on("destroy", [this](const EventArgs&) {
                _imageEntity = nullptr;
                _imageEntityDestroyed.reset();
            });
            if (auto* element = _imageEntity->findComponent<ElementComponent>()) {
                bindImageElement(element);
            }
        }
    }

    void ButtonComponent::setTransitionMode(const ButtonTransitionMode value)
    {
        if (_transitionMode == value) {
            return;
        }
        const ButtonTransitionMode oldMode = _transitionMode;
        _transitionMode = value;
        cancelTween();
        resetToDefaultVisualState(oldMode);
        forceReapplyVisualState();
    }

    void ButtonComponent::setTint(Color& tint, const Color& value)
    {
        if (tint == value) {
            return;
        }
        tint = value;
        forceReapplyVisualState();
    }

    void ButtonComponent::setHoverSprite(std::shared_ptr<Sprite> value)
    {
        if (_hoverSprite != value) {
            _hoverSprite = std::move(value);
            forceReapplyVisualState();
        }
    }

    void ButtonComponent::setHoverSpriteFrame(const int value)
    {
        if (_hoverSpriteFrame != value) {
            _hoverSpriteFrame = value;
            forceReapplyVisualState();
        }
    }

    void ButtonComponent::setPressedSprite(std::shared_ptr<Sprite> value)
    {
        if (_pressedSprite != value) {
            _pressedSprite = std::move(value);
            forceReapplyVisualState();
        }
    }

    void ButtonComponent::setPressedSpriteFrame(const int value)
    {
        if (_pressedSpriteFrame != value) {
            _pressedSpriteFrame = value;
            forceReapplyVisualState();
        }
    }

    void ButtonComponent::setInactiveSprite(std::shared_ptr<Sprite> value)
    {
        if (_inactiveSprite != value) {
            _inactiveSprite = std::move(value);
            forceReapplyVisualState();
        }
    }

    void ButtonComponent::setInactiveSpriteFrame(const int value)
    {
        if (_inactiveSpriteFrame != value) {
            _inactiveSpriteFrame = value;
            forceReapplyVisualState();
        }
    }

    // ---- lifecycle -------------------------------------------------------------------------

    void ButtonComponent::onEnable()
    {
        _isHovering = false;
        _isPressed = false;
        bindHitElement();
        if (_imageEntity && !_imageElement) {
            if (auto* element = _imageEntity->findComponent<ElementComponent>()) {
                bindImageElement(element);
            }
        }
        forceReapplyVisualState();
    }

    void ButtonComponent::onDisable()
    {
        unbindHitElement();
        resetToDefaultVisualState(_transitionMode);
    }

    void ButtonComponent::refreshBindings()
    {
        if (active() && !_hitElement) {
            bindHitElement();
        }
        if (_imageEntity && !_imageElement) {
            if (auto* element = _imageEntity->findComponent<ElementComponent>()) {
                bindImageElement(element);
            }
        }
    }

    void ButtonComponent::bindHitElement()
    {
        auto* element = _entity ? _entity->findComponent<ElementComponent>() : nullptr;
        if (!element || element == _hitElement) {
            return;
        }
        unbindHitElement();
        _hitElement = element;
        for (const char* name : kHitEvents) {
            _hitHandles.push_back(element->on(name, [this, name](const EventArgs& args) { onHitEvent(name, args); }));
        }
        _hitHandles.push_back(element->on("beforeremove", [this](const EventArgs&) { unbindHitElement(); }));
    }

    void ButtonComponent::unbindHitElement()
    {
        for (auto& handle : _hitHandles) {
            handle->off();
        }
        _hitHandles.clear();
        _hitElement = nullptr;
    }

    void ButtonComponent::bindImageElement(ElementComponent* element)
    {
        _imageElement = element;
        _imageHandles.push_back(element->on("beforeremove", [this](const EventArgs&) { unbindImageElement(true); }));
        // The application changing the image's look changes its DEFAULT look; the button's
        // own changes are told apart by the applying flags.
        const auto tintChanged = [this](const EventArgs&) {
            if (!_isApplyingTint) {
                storeDefaultVisualState();
                forceReapplyVisualState();
            }
        };
        const auto spriteChanged = [this](const EventArgs&) {
            if (!_isApplyingSprite) {
                storeDefaultVisualState();
                forceReapplyVisualState();
            }
        };
        _imageHandles.push_back(element->on("set:color", tintChanged));
        _imageHandles.push_back(element->on("set:opacity", tintChanged));
        _imageHandles.push_back(element->on("set:sprite", spriteChanged));
        _imageHandles.push_back(element->on("set:spriteFrame", spriteChanged));
        storeDefaultVisualState();
        forceReapplyVisualState();
    }

    void ButtonComponent::unbindImageElement(const bool resetVisual)
    {
        for (auto& handle : _imageHandles) {
            handle->off();
        }
        _imageHandles.clear();
        cancelTween();
        if (resetVisual) {
            resetToDefaultVisualState(_transitionMode);
        }
        _imageElement = nullptr;
    }

    void ButtonComponent::storeDefaultVisualState()
    {
        // A group element has no look to keep.
        if (!_imageElement || _imageElement->type() == ElementType::Group) {
            return;
        }
        const Color& color = _imageElement->color();
        _defaultTint = Color(color.r, color.g, color.b, _imageElement->opacity());
        _defaultSprite = _imageElement->sprite();
        _defaultSpriteFrame = _imageElement->spriteFrame();
    }

    // ---- input -----------------------------------------------------------------------------

    void ButtonComponent::onHitEvent(const char* name, const EventArgs& args)
    {
        const auto is = [name](const char* other) { return std::strcmp(name, other) == 0; };
        if (is("mouseenter")) {
            _isHovering = true;
        } else if (is("mouseleave")) {
            _isHovering = false;
            _isPressed = false;
        } else if (is("mousedown") || is("touchstart")) {
            _isPressed = true;
        } else if (is("mouseup") || is("touchend") || is("touchleave") || is("touchcancel")) {
            _isPressed = false;
        }
        if (!is("click")) {
            updateVisualState();
        }
        fireIfActive(name, inputEventOf(args));
    }

    void ButtonComponent::fireIfActive(const char* name, ElementInputEvent* event)
    {
        if (!_active) {
            return;
        }
        if (event) {
            fire(name, event);
        } else {
            fire(name);
        }
    }

    // ---- visual state ----------------------------------------------------------------------

    ButtonComponent::VisualState ButtonComponent::determineVisualState() const
    {
        if (!_active) {
            return VisualState::Inactive;
        }
        if (_isPressed) {
            return VisualState::Pressed;
        }
        if (_isHovering) {
            return VisualState::Hover;
        }
        return VisualState::Default;
    }

    void ButtonComponent::updateVisualState(const bool force)
    {
        const VisualState oldState = _visualState;
        const VisualState newState = determineVisualState();
        if ((oldState == newState && !force) || !enabled()) {
            return;
        }
        _visualState = newState;
        if (oldState == VisualState::Hover) {
            fireIfActive("hoverend", nullptr);
        }
        if (oldState == VisualState::Pressed) {
            fireIfActive("pressedend", nullptr);
        }
        if (newState == VisualState::Hover) {
            fireIfActive("hoverstart", nullptr);
        }
        if (newState == VisualState::Pressed) {
            fireIfActive("pressedstart", nullptr);
        }

        switch (_transitionMode) {
        case ButtonTransitionMode::Tint:
            switch (_visualState) {
            case VisualState::Default:  applyTint(_defaultTint); break;
            case VisualState::Hover:    applyTint(_hoverTint); break;
            case VisualState::Pressed:  applyTint(_pressedTint); break;
            case VisualState::Inactive: applyTint(_inactiveTint); break;
            }
            break;
        case ButtonTransitionMode::SpriteChange:
            switch (_visualState) {
            case VisualState::Default:  applySprite(_defaultSprite, _defaultSpriteFrame); break;
            case VisualState::Hover:    applySprite(_hoverSprite, _hoverSpriteFrame); break;
            case VisualState::Pressed:  applySprite(_pressedSprite, _pressedSpriteFrame); break;
            case VisualState::Inactive: applySprite(_inactiveSprite, _inactiveSpriteFrame); break;
            }
            break;
        }
    }

    void ButtonComponent::resetToDefaultVisualState(const ButtonTransitionMode mode)
    {
        if (!_imageElement) {
            return;
        }
        switch (mode) {
        case ButtonTransitionMode::Tint:
            cancelTween();
            applyTintImmediately(_defaultTint);
            break;
        case ButtonTransitionMode::SpriteChange:
            applySprite(_defaultSprite, _defaultSpriteFrame);
            break;
        }
    }

    void ButtonComponent::applySprite(const std::shared_ptr<Sprite>& sprite, const int frame)
    {
        if (!_imageElement) {
            return;
        }
        _isApplyingSprite = true;
        if (sprite && _imageElement->sprite() != sprite) {
            _imageElement->setSprite(sprite);
        }
        if (_imageElement->spriteFrame() != frame) {
            _imageElement->setSpriteFrame(frame);
        }
        _isApplyingSprite = false;
    }

    void ButtonComponent::applyTint(const Color& tint)
    {
        cancelTween();
        if (_fadeDuration == 0.0f) {
            applyTintImmediately(tint);
            return;
        }
        if (!_imageElement || _imageElement->type() == ElementType::Group) {
            return;
        }
        const Color& color = _imageElement->color();
        if (sameRgb(color, tint) && _imageElement->opacity() == tint.a) {
            return;
        }
        _tween = Tween{0.0f, Color(color.r, color.g, color.b, _imageElement->opacity()), tint};
    }

    void ButtonComponent::applyTintImmediately(const Color& tint)
    {
        if (!_imageElement || _imageElement->type() == ElementType::Group) {
            return;
        }
        _isApplyingTint = true;
        const Color& color = _imageElement->color();
        if (!sameRgb(color, tint)) {
            // Upstream sets a three-channel colour; the image's alpha is its opacity.
            _imageElement->setColor(Color(tint.r, tint.g, tint.b, color.a));
        }
        if (_imageElement->opacity() != tint.a) {
            _imageElement->setOpacity(tint.a);
        }
        _isApplyingTint = false;
    }

    void ButtonComponent::update(const float dt)
    {
        if (!_tween) {
            return;
        }
        _tween->elapsed += dt * 1000.0f;
        const float t = _fadeDuration == 0.0f ? 1.0f : std::clamp(_tween->elapsed / _fadeDuration, 0.0f, 1.0f);
        if (std::abs(t - 1.0f) > 1e-5f) {
            const Color& a = _tween->from;
            const Color& b = _tween->to;
            applyTintImmediately(Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                                       a.a + (b.a - a.a) * t));
        } else {
            const Color to = _tween->to;
            applyTintImmediately(to);
            cancelTween();
        }
    }
}
