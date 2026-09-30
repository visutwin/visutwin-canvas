// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A button (upstream framework/components/button/component.js): it turns the input events
// its entity's element receives into a visual state — default, hover, pressed or inactive —
// and shows that state on an IMAGE element, its `imageEntity`, by tinting it or by changing
// its sprite.
//
// The button re-fires its element's mouse and touch events and `click` while it is active,
// and fires `hoverstart`, `hoverend`, `pressedstart` and `pressedend` as the state changes.
// Handlers get the element's `ElementInputEvent*`:
//
//     button->on("click", [](ElementInputEvent* event) { ... });
//
// The image's own colour, opacity, sprite and frame are its DEFAULT look: the button records
// them when it gains the image and whenever the application changes them, and puts them back
// when it is disabled or lets go of the image.
//
// DEVIATIONS:
// - the state sprites are Sprites, not sprite assets, and a null one keeps the image's
//   sprite (upstream assigns the null asset, which matches its examples only because they
//   set the image's `sprite` directly, leaving its `spriteAsset` null too);
// - the tint fade advances with the engine's frame time, not the wall clock, so it is
//   deterministic under a fixed time step;
// - `hitPadding` and the tints are plain values: upstream's null-passing setters have no
//   counterpart.
//
#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector4.h"
#include "framework/components/component.h"
#include "framework/components/componentInstanceList.h"

namespace visutwin::canvas
{
    class ElementComponent;
    class Entity;
    class Sprite;
    struct ElementInputEvent;

    /// Upstream BUTTON_TRANSITION_MODE_TINT / _SPRITE_CHANGE.
    enum class ButtonTransitionMode
    {
        Tint = 0,
        SpriteChange = 1
    };

    class ButtonComponent : public Component
    {
    public:
        enum class VisualState
        {
            Default,
            Hover,
            Pressed,
            Inactive
        };

        ButtonComponent(IComponentSystem* system, Entity* entity);
        ~ButtonComponent() override;

        /// Binds the entity's element: adding a component does not call onEnable here.
        void initializeComponentData() override { refreshBindings(); }
        void cloneFrom(const Component* source) override;
        void resolveClonedReferences(const Component* source, const CloneNodeMap& map) override;
        void onEnable() override;
        void onDisable() override;

        static const std::vector<ButtonComponent*>& instances() { return _instanceList.items(); }

        /// Upstream `active`: an inactive button shows its inactive state and fires no
        /// events. DEVIATION in name only: `active()` is Component's "enabled here and in
        /// every parent", which this must not hide.
        bool isActive() const { return _active; }
        void setActive(bool value);

        /// The entity whose IMAGE element shows the state (often the button's own entity).
        /// Dropped (reads null) when that entity is destroyed.
        Entity* imageEntity() const { return _imageEntity; }
        void setImageEntity(Entity* entity);

        /// How far the area that takes input reaches beyond the element: left, bottom, right,
        /// top, in the element's units.
        const Vector4& hitPadding() const { return _hitPadding; }
        void setHitPadding(const Vector4& value) { _hitPadding = value; }

        ButtonTransitionMode transitionMode() const { return _transitionMode; }
        void setTransitionMode(ButtonTransitionMode value);

        /// Tints REPLACE the image's colour; alpha becomes its opacity. The default state
        /// shows the image's own colour and opacity.
        const Color& hoverTint() const { return _hoverTint; }
        void setHoverTint(const Color& value) { setTint(_hoverTint, value); }
        const Color& pressedTint() const { return _pressedTint; }
        void setPressedTint(const Color& value) { setTint(_pressedTint, value); }
        const Color& inactiveTint() const { return _inactiveTint; }
        void setInactiveTint(const Color& value) { setTint(_inactiveTint, value); }
        /// How long a tint change fades over, in MILLISECONDS as upstream; 0 is immediate.
        float fadeDuration() const { return _fadeDuration; }
        void setFadeDuration(const float value) { _fadeDuration = std::max(value, 0.0f); }

        const std::shared_ptr<Sprite>& hoverSprite() const { return _hoverSprite; }
        void setHoverSprite(std::shared_ptr<Sprite> value);
        int hoverSpriteFrame() const { return _hoverSpriteFrame; }
        void setHoverSpriteFrame(int value);
        const std::shared_ptr<Sprite>& pressedSprite() const { return _pressedSprite; }
        void setPressedSprite(std::shared_ptr<Sprite> value);
        int pressedSpriteFrame() const { return _pressedSpriteFrame; }
        void setPressedSpriteFrame(int value);
        const std::shared_ptr<Sprite>& inactiveSprite() const { return _inactiveSprite; }
        void setInactiveSprite(std::shared_ptr<Sprite> value);
        int inactiveSpriteFrame() const { return _inactiveSpriteFrame; }
        void setInactiveSpriteFrame(int value);

        VisualState visualState() const { return _visualState; }

        /// Advance a tint fade (upstream `onUpdate`); the button system calls it every update.
        void update(float dt);
        /// Pick up an element added to this entity or to the image entity since the button
        /// last looked (upstream listens for `element:add`, which nothing fires here). The
        /// system calls it every update; it costs nothing once both are bound.
        void refreshBindings();

    private:
        void setTint(Color& tint, const Color& value);

        void bindHitElement();
        void unbindHitElement();
        void bindImageElement(ElementComponent* element);
        void unbindImageElement(bool resetVisual);
        void storeDefaultVisualState();

        void onHitEvent(const char* name, const EventArgs& args);
        void fireIfActive(const char* name, ElementInputEvent* event);

        void updateVisualState(bool force = false);
        void forceReapplyVisualState() { updateVisualState(true); }
        void resetToDefaultVisualState(ButtonTransitionMode mode);
        VisualState determineVisualState() const;
        void applySprite(const std::shared_ptr<Sprite>& sprite, int frame);
        void applyTint(const Color& tint);
        void applyTintImmediately(const Color& tint);
        void cancelTween() { _tween.reset(); }

        inline static ComponentInstanceList<ButtonComponent> _instanceList;

        bool _active = true;
        Entity* _imageEntity = nullptr;
        EventHandlePtr _imageEntityDestroyed;
        Vector4 _hitPadding = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        ButtonTransitionMode _transitionMode = ButtonTransitionMode::Tint;
        Color _hoverTint = Color(0.75f, 0.75f, 0.75f, 1.0f);
        Color _pressedTint = Color(0.5f, 0.5f, 0.5f, 1.0f);
        Color _inactiveTint = Color(0.25f, 0.25f, 0.25f, 1.0f);
        float _fadeDuration = 0.0f;
        std::shared_ptr<Sprite> _hoverSprite;
        int _hoverSpriteFrame = 0;
        std::shared_ptr<Sprite> _pressedSprite;
        int _pressedSpriteFrame = 0;
        std::shared_ptr<Sprite> _inactiveSprite;
        int _inactiveSpriteFrame = 0;

        bool _isHovering = false;
        bool _isPressed = false;
        VisualState _visualState = VisualState::Default;

        Color _defaultTint = Color(1.0f, 1.0f, 1.0f, 1.0f);
        std::shared_ptr<Sprite> _defaultSprite;
        int _defaultSpriteFrame = 0;
        bool _isApplyingTint = false;
        bool _isApplyingSprite = false;

        struct Tween
        {
            float elapsed = 0.0f;   // milliseconds
            Color from;
            Color to;
        };
        std::optional<Tween> _tween;

        ElementComponent* _hitElement = nullptr;
        std::vector<EventHandlePtr> _hitHandles;
        ElementComponent* _imageElement = nullptr;
        std::vector<EventHandlePtr> _imageHandles;
    };
}
