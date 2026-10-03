// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 13.10.2025
//
#pragma once

#include <SDL3/SDL_events.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/color.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/shape/boundingBox.h"
#include "framework/components/element/elementComponent.h"
#include "framework/input/uiGeometryArena.h"
#include "platform/graphics/stencilParameters.h"
#include "platform/input/inputConstants.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Engine;
    class ElementComponent;
    class Entity;
    class Mesh;
    class Material;
    class MeshInstance;
    class RenderComponent;
    class Sprite;
    class StandardMaterial;
    class Texture;

    /**
     * An input event delivered to an element, with the mouse and touch fields in one
     * struct. It is fired as a POINTER,
     * `ElementInputEvent*`, so every handler on the way up the hierarchy sees the same event
     * and `stopPropagation()` from one of them stops the bubbling:
     *
     *     element->on("click", [](ElementInputEvent* event) { event->stopPropagation(); });
     *
     * A handler that takes no arguments works too. The pointer is valid only during the call.
     */
    struct ElementInputEvent
    {
        /// The element the event is for; it bubbles to the elements of the parent entities.
        ElementComponent* element = nullptr;
        /// The camera the element was hit through.
        CameraComponent* camera = nullptr;
        /// Canvas position in points, y down from the top (the space SDL's mouse uses).
        float x = 0.0f;
        float y = 0.0f;
        /// Mouse movement since the previous mouse event.
        float dx = 0.0f;
        float dy = 0.0f;
        MouseButton button = MouseButton::None;
        /// The wheel's direction only, -1 or 1.
        int wheelDelta = 0;
        /// The wheel's movement as a browser's WheelEvent gives it (what the scroll view
        /// reads): pixels, x positive to the right and y positive TOWARD the user. DEVIATION:
        /// SDL gives notches, and a notch is taken as 100 pixels.
        float wheelPixelsX = 0.0f;
        float wheelPixelsY = 0.0f;
        KeyModifiers modifiers;
        /// A touch event, and the finger it is for.
        bool touch = false;
        int64_t touchId = 0;

        /// The delta on the old wheel scale, -2 per notch away from the user.
        float wheel() const { return static_cast<float>(wheelDelta) * -2.0f; }
        void stopPropagation() { _stopPropagation = true; }
        bool propagationStopped() const { return _stopPropagation; }

    private:
        bool _stopPropagation = false;
    };

    /**
     * Draws UI elements and delivers input to them. Each text or image element gets a
     * visual: a child entity with a render component, rebuilt when what it depends on
     * changes. Engine::render syncs the visuals before drawing, so an application does not
     * call it.
     *
     * Input: Engine::handleInputEvent passes every SDL event to `handleEvent`, which turns
     * mouse and touch events into the calls below; a platform without SDL calls them
     * directly. Elements with `useInput` receive `mousedown`, `mouseup`, `mousemove`,
     * `mousewheel`, `mouseenter`, `mouseleave`, `click`, `touchstart`, `touchmove`,
     * `touchend`, `touchleave` and `touchcancel`, each bubbling to the parent entities'
     * elements. The element hit is the front one under the pointer:
     * cameras from the last drawn back, and per camera the elements on layers it draws,
     * front layer first, screen-space before world-space, higher draw order first. A
     * screen-space element is hit through its screen corners, any other by a ray from the
     * camera through its world corners; a button's hit padding grows either.
     */
    class ElementInput
    {
    public:
        /// The colour, outline and shadow one run of text draws in.
        struct TextStyle
        {
            Color color;
            Color outlineColor;
            float outlineThickness = 0.0f;
            Color shadowColor;
            Vector2 shadowOffset;
        };

        ElementInput() = default;
        ~ElementInput();
        ElementInput(const ElementInput&) = delete;
        ElementInput& operator=(const ElementInput&) = delete;

        void setEngine(const std::shared_ptr<Engine>& engine) { _engine = engine; }

        void detach();

        /// While false, no input event is delivered.
        bool enabled() const { return _enabled; }
        void setEnabled(const bool value) { _enabled = value; }

        /// Translate an SDL mouse or finger event. Touch comes only from DIRECT touch
        /// devices (a trackpad's fingers are not touches on the canvas), and the mouse
        /// events SDL synthesizes from touches are dropped, as a browser's are by a
        /// button's `preventDefault`. Returns true when a handler called
        /// `stopPropagation()` on an event it produced, which keeps the press from the mouse
        /// and touch devices too: Engine::handleInputEvent withholds it from them.
        bool handleEvent(const SDL_Event& event);

        // Platform-neutral input, in canvas points (y down).
        void onMouseDown(float x, float y, MouseButton button, const KeyModifiers& modifiers = {});
        void onMouseUp(float x, float y, MouseButton button, const KeyModifiers& modifiers = {});
        void onMouseMove(float x, float y, const KeyModifiers& modifiers = {});
        /// In notches: `deltaY` > 0 is away from the user, `deltaX` > 0 to the right.
        void onMouseWheel(float x, float y, float deltaY, const KeyModifiers& modifiers = {}, float deltaX = 0.0f);
        void onTouchStart(int64_t id, float x, float y);
        void onTouchMove(int64_t id, float x, float y);
        void onTouchEnd(int64_t id, float x, float y);
        void onTouchCancel(int64_t id, float x, float y);

        /// The element under the pointer, and the one a mouse button went down on.
        ElementComponent* hoveredElement() const { return _hoveredElement; }
        ElementComponent* pressedElement() const { return _pressedElement; }

        /// The front input element at canvas point (x, y) seen through `camera`, or null.
        ElementComponent* elementAt(CameraComponent* camera, float x, float y);

        /// The corners an element is hit through: `corners` (screen or world) grown by its
        /// button's hit padding, scaled by `scale`, and reordered for negative scales.
        static std::array<Vector3, 4> buildHitCorners(ElementComponent* element, const std::array<Vector3, 4>& corners,
                                                      const Vector3& scale);
        /// Create, update and retire the visuals of every text and image element.
        void syncElements();

    private:
    public:
        /// Everything an element material is built from. Two parts with equal keys draw
        /// with ONE material: the
        /// renderer skips the material bind between consecutive draws of one material,
        /// and a screen of labels in one font and colour packs one uniform block, not one
        /// per label. All values are the ones the material is given, already converted
        /// (outline thickness x 0.2, shadow offset in atlas UV).
        struct MaterialKey
        {
            enum class Kind : uint8_t { Image, ImageMask, BitmapText, MsdfText };

            Kind kind = Kind::Image;
            bool worldSpace = false;
            Texture* texture = nullptr;
            Color color;
            float opacity = 1.0f;
            // MSDF text only; left at these defaults otherwise.
            float pxRange = 0.0f;
            float intensity = 0.0f;
            Color outlineColor;
            float outlineThickness = 0.0f;
            Color shadowColor;
            Vector2 shadowUvOffset;

            bool operator==(const MaterialKey& other) const;
            /// Whether a material built for `other` becomes this one by restyling it
            /// (colour, opacity, outline, shadow) rather than by building another.
            [[nodiscard]] bool sameBuild(const MaterialKey& other) const;
        };

        struct MaterialKeyHash
        {
            size_t operator()(const MaterialKey& key) const;
        };

    private:
        /// One mesh instance of a visual: an image has one, text one per (atlas page,
        /// markup style) run.
        struct VisualPart
        {
            /// The part's own Mesh object; its buffers and index range are `geometry`'s.
            std::shared_ptr<Mesh> mesh;
            /// Where the geometry lives in the shared UI buffers. Replaced, not rewritten,
            /// when the element is resized.
            std::unique_ptr<UiGeometryArena::Block> geometry;
            /// Shared with every part whose key equals `materialKey`; null for a custom
            /// material.
            std::shared_ptr<StandardMaterial> material;
            MaterialKey materialKey;
            /// An image element's own `material()`, drawn instead of `material` (which is then
            /// null): its colour, opacity and texture are its own affair.
            std::shared_ptr<Material> customMaterial;
            MeshInstance* meshInstance = nullptr;
            Texture* texture = nullptr;
            /// Text: the atlas page this run draws from.
            int page = 0;
            /// Index into ElementVisual::styles; 0 is the element's own, read live.
            int style = 0;
            /// Text: the symbol of each quad, ascending, for the draw range.
            std::vector<uint32_t> quadSymbols;
        };

        struct ElementVisual
        {
            Entity* entity = nullptr;
            RenderComponent* render = nullptr;
            std::vector<VisualPart> parts;
            /// Text: the styles its parts draw in (resolved at the last rebuild).
            std::vector<TextStyle> styles;
            ElementType type = ElementType::Group;
            /// The sync that last saw the element (ElementInput::_syncSerial).
            uint64_t syncSerial = 0;
            // Decided once, when the visual is created: an element on a screen-space
            // screen is drawn in screen space (depth test off, over whatever layer it is
            // on); any other is world geometry that simply follows its entity.
            bool worldSpace = false;
            std::vector<int> layers;
            EventHandlePtr destroyHandle;

            // What the mesh was built from; a difference rebuilds it.
            float cachedWidth = 0.0f;
            float cachedHeight = 0.0f;
            Vector2 cachedPivot = Vector2(0.5f, 0.5f);
            uint64_t cachedRangeVersion = 0;
            /// A mask's second draw, after its last descendant, which puts the stencil back;
            /// null for anything that is not a mask.
            MeshInstance* unmask = nullptr;
            bool cachedMask = false;
            // image
            uint64_t cachedImageVersion = 0;
            const Sprite* cachedSprite = nullptr;
            uint64_t cachedSpriteVersion = 0;
            uint64_t cachedAtlasVersion = 0;
        };

        struct TouchInfo
        {
            ElementComponent* element = nullptr;
            CameraComponent* camera = nullptr;
            float x = 0.0f;
            float y = 0.0f;
        };

        /// Run over every element tree before each frame: who masks
        /// whom, each draw's stencil state, and where each unmask draws.
        void syncMasks();
        /// One shared parameter set per (test, write, reference).
        std::shared_ptr<StencilParameters> stencilParameters(StencilCompareFunction func, StencilOperation pass,
                                                             uint32_t ref);

        ElementVisual& visualFor(ElementComponent* element);
        void releaseVisual(ElementVisual& visual);
        /// Puts the geometry in the shared buffers and points the part's mesh at it. False,
        /// leaving the part as it was, when there is nothing to draw.
        bool setPartGeometry(VisualPart& part, const std::vector<float>& vertices, const std::vector<uint32_t>& indices,
                             const BoundingBox& bounds);
        /// The key of the material this part should draw with right now.
        static MaterialKey materialKeyFor(const ElementVisual& visual, const VisualPart& part,
                                          const ElementComponent* element);
        /// Gives the part (and `unmask`, a mask's second draw) the material for `key`: the
        /// one another part already has, its own restyled when nothing shares it, or a new
        /// one. Nothing happens while the key is unchanged.
        void applyPartMaterial(VisualPart& part, const MaterialKey& key, MeshInstance* unmask);
        /// Every part's material and draw order from the element's current state.
        void styleParts(ElementVisual& visual, const ElementComponent* element);

        void onElementMouseEvent(const char* eventType, float x, float y, MouseButton button, int wheelDelta,
                                 const KeyModifiers& modifiers);
        /// The element under (x, y) through the cameras from the last drawn back, and the
        /// camera it was hit through.
        std::pair<ElementComponent*, CameraComponent*> targetAt(float x, float y);
        std::vector<CameraComponent*> sortedCameras() const;
        /// Fire `name` at the event's element and on up the parent entities' elements.
        void fireEvent(const char* name, ElementInputEvent& event);
        /// Forget an element that is going away, so no event is sent to it later.
        void forgetElement(ElementComponent* element);
        void watchElement(ElementComponent* element);

        std::shared_ptr<Engine> _engine;
        std::unordered_map<ElementComponent*, ElementVisual> _visuals;
        /// The buffers every visual's geometry lives in; made by the first sync.
        std::shared_ptr<UiGeometryArena> _geometry;
        /// The element materials alive right now, by what they are. Weak: the parts own
        /// them, and the last part to let go of one frees it.
        std::unordered_map<MaterialKey, std::weak_ptr<StandardMaterial>, MaterialKeyHash> _materials;
        size_t _materialPruneAt = 64;
        /// Counts syncElements calls; a visual whose stamp is older was not seen.
        uint64_t _syncSerial = 0;
        /// The last syncMasks run left stencil state or `maskedBy` set somewhere.
        bool _masksApplied = false;
        std::unordered_map<uint64_t, std::shared_ptr<StencilParameters>> _stencilCache;

        bool _enabled = true;
        /// A handler stopped an event since handleEvent began.
        bool _propagationStopped = false;
        float _lastX = 0.0f;
        // The wheel event being delivered, in pixels (ElementInputEvent::wheelPixelsX/Y).
        float _wheelPixelsX = 0.0f;
        float _wheelPixelsY = 0.0f;
        float _lastY = 0.0f;
        ElementComponent* _hoveredElement = nullptr;
        ElementComponent* _pressedElement = nullptr;
        std::unordered_map<int64_t, TouchInfo> _touchedElements;
        std::unordered_map<int64_t, bool> _touchLeaveFired;
        /// When a touch last clicked an element, so the mouse
        /// click a platform synthesizes from it is not delivered a second time.
        std::unordered_map<const ElementComponent*, std::chrono::steady_clock::time_point> _clickedElements;
        /// A `destroy` subscription per element the input remembers (hovered, pressed,
        /// touched), so a destroyed element is forgotten rather than dangling.
        std::unordered_map<ElementComponent*, EventHandlePtr> _watched;
    };
}
