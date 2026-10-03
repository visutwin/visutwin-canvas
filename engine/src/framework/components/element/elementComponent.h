// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// A UI element, its LAYOUT: an
// element sits in its parent element's rectangle, or its screen's, by ANCHORS (fractions
// of that rectangle, x/y the bottom-left corner and z/w the top-right), a PIVOT (the point
// of the element its position names, 0..1) and MARGINS (distances from the anchors: left,
// bottom, right, top). On an axis whose two anchors coincide the element keeps its own
// width or height and its position places it; on a "split" axis it stretches between the
// anchors and the margins set its size.
//
// Its entity's transform is computed by the element while it has a screen
// (GraphNodeTransformHook): the world transform goes through
// the anchors, the parent element's model transform and the screen's projection, so a
// SCREEN-SPACE element's world transform is in clip space and a camera that draws it maps
// world XY straight to NDC; a world-space screen's elements are ordinary world geometry.
// Setting the entity's position re-derives the margins.
//
// Configure an element either with setters, or all at once with `setup(ElementDesc)`,
// which applies the values in a fixed order.
//
// Drawing is ElementInput's (text and images, synced by Engine::render). An image element
// takes a texture with a UV rect, or a sprite frame, simple or 9-sliced
// (imageElementGeometry.h); its colour multiplies the texture.
//
// Text is laid out (textLayout.h) as soon as an input changes, so its
// size can be read at once; with autoWidth / autoHeight (the default) the element takes it.
// `enableMarkup` reads `[color]`, `[outline]` and `[shadow]` tags (markup.h).
//
// Not ported yet: masks, layout groups, batching, max lines, auto-fit font size and
// right-to-left reordering.
//
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/eventHandler.h"
#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/math/vector4.h"
#include "framework/components/component.h"
#include "framework/components/element/imageElementGeometry.h"
#include "framework/components/element/markup.h"
#include "framework/components/element/textLayout.h"
#include "framework/handlers/fontResource.h"
#include "scene/graphNodeTransformHook.h"
#include "framework/components/componentInstanceList.h"

namespace visutwin::canvas
{
    class Material;
    class ScreenComponent;
    class Sprite;
    class Texture;

    /// A group lays out and draws nothing.
    enum class ElementType
    {
        Group,
        Image,
        Text
    };

    enum class ElementHorizontalAlign
    {
        Left,
        Center,
        Right
    };

    /// The fields given to `setup`, applied in a fixed order.
    struct ElementDesc
    {
        std::optional<ElementType> type;
        std::optional<Vector4> anchor;
        std::optional<Vector2> pivot;
        std::optional<Vector4> margin;
        std::optional<float> left;
        std::optional<float> bottom;
        std::optional<float> right;
        std::optional<float> top;
        std::optional<float> width;
        std::optional<float> height;
        std::optional<bool> useInput;
    };

    class ElementComponent : public Component, public GraphNodeTransformHook
    {
    public:
        ElementComponent(IComponentSystem* system, Entity* entity);
        ~ElementComponent() override;

        /// `setup({})`: derives the margins from the entity's position and finds a screen.
        void initializeComponentData() override;
        void cloneFrom(const Component* source) override;

        /// Initializes the element from `desc`.
        void setup(const ElementDesc& desc);

        static const std::vector<ElementComponent*>& instances() { return _instanceList.items(); }
        /// Unique for the life of the process (an address can be reused by a later element);
        /// what a layout group tells its children apart by.
        uint64_t serial() const { return _serial; }

        ElementType type() const { return _type; }
        void setType(ElementType value) { _type = value; textChanged(); }

        // ---- layout --------------------------------------------------------------------

        const Vector4& anchor() const { return _anchor; }
        void setAnchor(const Vector4& value);

        const Vector2& pivot() const { return _pivot; }
        void setPivot(const Vector2& value);

        /// left, bottom, right, top.
        const Vector4& margin() const { return _margin; }
        void setMargin(const Vector4& value);

        float left() const { return _margin.getX(); }
        void setLeft(float value);
        float bottom() const { return _margin.getY(); }
        void setBottom(float value);
        float right() const { return _margin.getZ(); }
        void setRight(float value);
        float top() const { return _margin.getW(); }
        void setTop(float value);

        /// The authored size; ignored on a split axis, where the anchors and margins rule.
        float width() const { return _width; }
        void setWidth(float value);
        float height() const { return _height; }
        void setHeight(float value);

        /// The size the element actually has.
        float calculatedWidth() const { return _calculatedWidth; }
        void setCalculatedWidth(float value) { setCalculatedWidthInternal(value, true); }
        float calculatedHeight() const { return _calculatedHeight; }
        void setCalculatedHeight(float value) { setCalculatedHeightInternal(value, true); }

        /// The screen ENTITY this element is laid out on, or null.
        Entity* screen() const { return _screen; }
        ScreenComponent* screenComponent() const;

        /// Corners relative to the screen (bottom left, bottom right, top right, top left),
        /// in resolution units with y up from the bottom.
        const std::array<Vector3, 4>& screenCorners();
        /// Canvas corners: screen corners with y DOWN from the top — window points for a
        /// screen-space screen, the space mouse coordinates are in.
        const std::array<Vector2, 4>& canvasCorners();
        /// The corners in world space.
        const std::array<Vector3, 4>& worldCorners();

        /// The transform from the element's local space to the screen's model space.
        const Matrix4& modelTransform() const { return _modelTransform; }

        // ---- screen callbacks -----------------------------------------------------------

        void updateScreen(Entity* screen);
        void onScreenResize(const Vector2& resolution);
        void onScreenRemove(ScreenComponent* screen);

        // ---- appearance, read by ElementInput ------------------------------------------

        // The colour, opacity, sprite and sprite frame setters fire `set:color`,
        // `set:opacity`, `set:sprite` and `set:spriteFrame` when the value changes; a button
        // keeps its image's default look through them.
        float opacity() const { return _opacity; }
        void setOpacity(float value);
        const Color& color() const { return _color; }
        void setColor(const Color& value);
        /// The size the text is drawn at: under auto fit, the size the fit chose; otherwise
        /// the size set.
        int fontSize() const { return shouldAutoFit() ? _fittedFontSize : _fontSize; }
        /// The size to draw at when not auto fitting (kept while a fit is on).
        void setFontSize(const int value) { _fontSize = std::max(value, 1); textChanged(); }
        /// Shrink the font, from maxFontSize down to minFontSize, until the text fits the
        /// element's width / height. Each works only while the matching autoWidth / autoHeight
        /// is off.
        bool autoFitWidth() const { return _autoFitWidth; }
        void setAutoFitWidth(const bool value) { _autoFitWidth = value; textChanged(); }
        bool autoFitHeight() const { return _autoFitHeight; }
        void setAutoFitHeight(const bool value) { _autoFitHeight = value; textChanged(); }
        int minFontSize() const { return _minFontSize; }
        void setMinFontSize(const int value) { _minFontSize = value; textChanged(); }
        int maxFontSize() const { return _maxFontSize; }
        void setMaxFontSize(const int value) { _maxFontSize = value; textChanged(); }
        /// A wrapping text stops breaking lines once it has this many, and
        /// the rest runs on in the last one. Negative (the default) for no limit; ignored for text
        /// that does not wrap.
        int maxLines() const { return _maxLines; }
        void setMaxLines(const int value) { _maxLines = value < 0 ? -1 : value; textChanged(); }
        bool shouldAutoFitWidth() const { return _autoFitWidth && !_autoWidth; }
        bool shouldAutoFitHeight() const { return _autoFitHeight && !_autoHeight; }
        bool shouldAutoFit() const { return shouldAutoFitWidth() || shouldAutoFitHeight(); }
        /// The layout at the current (fitted) size: what the element measures itself by and its
        /// visual places glyphs by.
        TextMeasure measureLayout() const;
        const std::string& text() const { return _text; }
        /// Plain text; clears the localization key.
        void setText(const std::string& value) { _i18nKey.clear(); _text = value; textChanged(); }
        /// The text is the engine's I18n message for this key in the current
        /// locale, and follows the locale and any data added for it. Empty for none.
        const std::string& key() const { return _i18nKey; }
        void setKey(const std::string& value);
        FontResource* fontResource() const { return _fontResource; }
        void setFontResource(FontResource* value) { _fontResource = value; textChanged(); }
        ElementHorizontalAlign horizontalAlign() const { return _horizontalAlign; }
        void setHorizontalAlign(const ElementHorizontalAlign value) { _horizontalAlign = value; _textDirty = true; }
        /// Where the block of lines sits vertically in the box: 0 bottom, 0.5 centre (the
        /// default), 1 top. The block is measured from the font's glyph bounds.
        float verticalAlign() const { return _verticalAlign; }
        void setVerticalAlign(const float value) { _verticalAlign = std::clamp(value, 0.0f, 1.0f); _textDirty = true; }
        /// Text outline (thickness 0..1). MSDF fonts only.
        const Color& outlineColor() const { return _outlineColor; }
        void setOutlineColor(const Color& value) { _outlineColor = value; styleChanged(); }
        float outlineThickness() const { return _outlineThickness; }
        void setOutlineThickness(const float value) { _outlineThickness = value; styleChanged(); }
        /// Text drop shadow (the offset in editor units: a shift of 0.005 of the atlas
        /// width per unit). MSDF fonts only.
        const Color& shadowColor() const { return _shadowColor; }
        void setShadowColor(const Color& value) { _shadowColor = value; styleChanged(); }
        const Vector2& shadowOffset() const { return _shadowOffset; }
        void setShadowOffset(const Vector2& value) { _shadowOffset = value; styleChanged(); }
        bool wrapLines() const { return _wrapLines; }
        void setWrapLines(const bool value) { _wrapLines = value; textChanged(); }

        /// Multiplies every glyph's advance (1 = the font's own).
        float spacing() const { return _spacing; }
        void setSpacing(const float value) { _spacing = value; textChanged(); }
        /// The distance between lines; unset, the font size.
        /// Under auto fit the lines step by this scaled by the fitted size over maxFontSize.
        float lineHeight() const { return _lineHeight.value_or(static_cast<float>(_fontSize)); }
        void setLineHeight(const float value) { _lineHeight = value; textChanged(); }
        /// `enableMarkup`: read `[color]`, `[outline]` and `[shadow]` tags in the
        /// text (markup.h). An error draws the text as written, tags included.
        /// `justify`: wrapped lines stretch flush to both edges by widening their
        /// word gaps; lines ended by a line break and the last line keep the alignment.
        bool justify() const { return _justify; }
        void setJustify(const bool value)
        {
            if (value != _justify) {
                _justify = value;
                textChanged();
            }
        }
        /// Only the symbols (code points) in
        /// [rangeStart, rangeEnd) are drawn, without laying the text out again. Laying it out
        /// (a new text, font, size, width ...) resets the range to the whole text, so
        /// `rangeEnd()` right after `setText` is the text's length.
        int rangeStart() const { return _rangeStart; }
        void setRangeStart(int value);
        int rangeEnd() const { return _rangeEnd; }
        void setRangeEnd(int value);
        /// Bumped whenever the drawn range changes; ElementInput re-applies it.
        uint64_t rangeVersion() const { return _rangeVersion; }
        bool enableMarkup() const { return _enableMarkup; }
        void setEnableMarkup(const bool value) { _enableMarkup = value; textChanged(); }
        /// `autoWidth` / `autoHeight` (both on by default): the element takes the
        /// text's size on that axis unless its anchors split it. A text that wraps must turn
        /// autoWidth off, or it has no width to wrap at.
        bool autoWidth() const { return _autoWidth; }
        void setAutoWidth(const bool value) { _autoWidth = value; textChanged(); }
        bool autoHeight() const { return _autoHeight; }
        void setAutoHeight(const bool value) { _autoHeight = value; textChanged(); }

        /// The text drawn — markup stripped when it is on, a single space for an empty
        /// text — and each symbol's tags (empty without markup or tags).
        const std::string& textSymbols() const { return _symbols; }
        /// The same text as code points, what the layout places; markupTags() is indexed by
        /// these.
        const std::u32string& textCodePoints() const { return _codePoints; }
        const std::vector<std::optional<MarkupTags>>& markupTags() const { return _markupTags; }
        /// The text's own size, measured whenever
        /// the text, font, size, line height, wrapping or wrap width changes.
        float textWidth() const { return _textWidth; }
        float textHeight() const { return _textHeight; }
        /// The width lines wrap at: the element's, when wrapLines is on and the width is not
        /// automatic on an unsplit axis; unlimited otherwise.
        float textMaxLineWidth() const;
        /// The layers the element's visual is drawn on. Empty, the
        /// default, lets the element system choose: LAYERID_UI for an element on a screen of
        /// either kind, whose manual sort by draw order keeps a world-space
        /// screen's coplanar elements in order (on WORLD they sorted by distance and a panel
        /// could cover its own buttons). DEVIATION: an element on NO screen goes to
        /// LAYERID_WORLD, where upstream still says UI.
        const std::vector<int>& layers() const { return _layers; }
        void setLayers(const std::vector<int>& value) { _layers = value; }
        bool useInput() const { return _useInput; }
        void setUseInput(const bool value) { _useInput = value; }

        // ---- image ----------------------------------------------------------------------

        /// A custom material on an image element (a ShaderMaterial, say) that draws the
        /// element's quad instead of the element's own. It REPLACES the element's
        /// handling of colour, opacity and texture, which then belong to the material.
        /// Null (the default) draws the element's own material.
        const std::shared_ptr<Material>& material() const { return _customMaterial; }
        void setMaterial(std::shared_ptr<Material> value)
        {
            _customMaterial = std::move(value);
            ++_imageVersion;
        }
        /// A texture drawn over the element, through `rect`. Setting one clears the sprite.
        /// Borrowed: whoever loaded it must outlive the element.
        Texture* texture() const { return _texture; }
        void setTexture(Texture* value);
        /// A sprite drawn over the element; setting one clears the texture.
        const std::shared_ptr<Sprite>& sprite() const { return _sprite; }
        void setSprite(std::shared_ptr<Sprite> value);
        /// Which of the sprite's frames is drawn: clamped to the sprite's frames when it is
        /// set, when a sprite is assigned, and when the sprite's frame keys change (seen by
        /// `clampSpriteFrame`, which the element drawer calls before it builds the image).
        int spriteFrame() const { return _spriteFrame; }
        void setSpriteFrame(int value);
        /// Clamps the frame to the current sprite's frame count, without firing
        /// `set:spriteFrame`; for when the sprite's frames changed under the element.
        void clampSpriteFrame();
        /// The part of the texture drawn: x, y (from the bottom), width, height, as
        /// fractions of the texture. Ignored when a sprite is set.
        const Vector4& rect() const { return _rect; }
        void setRect(const Vector4& value) { _rect = value; ++_imageVersion; }
        /// Overrides the sprite's pixels per unit for a sliced sprite (null = the sprite's).
        std::optional<float> pixelsPerUnit() const { return _pixelsPerUnit; }
        void setPixelsPerUnit(const std::optional<float> value) { _pixelsPerUnit = value; ++_imageVersion; }
        /// An image element that MASKS its descendants. It is not drawn
        /// itself; its opaque texels (alpha test 1, so a sprite's transparent corners shape
        /// it) mark the stencil, and every element below it draws only there. Masks nest.
        bool mask() const { return _mask; }
        void setMask(const bool value)
        {
            if (value != _mask) {
                _mask = value;
                ++_imageVersion;
            }
        }
        /// The nearest mask above this element, or null. Worked out by ElementInput before
        /// each frame; a hit test on this
        /// element must also hit it.
        ElementComponent* maskedBy() const { return _maskedBy; }

        /// The record whoever draws this element keeps for it (ElementInput's visual),
        /// so the drawer reaches it without a lookup per element per frame. Tagged with
        /// its owner — another drawer reads null — and never copied by a clone.
        [[nodiscard]] void* drawRecord(const void* owner) const
        {
            return _drawRecordOwner == owner ? _drawRecord : nullptr;
        }
        void setDrawRecord(const void* owner, void* record)
        {
            _drawRecordOwner = record ? owner : nullptr;
            _drawRecord = record;
        }
        void setMaskedBy(ElementComponent* value)
        {
            _maskedBy = value;
            if (value) {
                value->_wasUsedAsMask = true;
            }
        }

        /// How the image keeps its aspect inside the rectangle.
        ElementFitMode fitMode() const { return _fitMode; }
        void setFitMode(const ElementFitMode value) { _fitMode = value; ++_imageVersion; }
        /// Bumped by every image setter; ElementInput rebuilds the geometry when it moves.
        uint64_t imageVersion() const { return _imageVersion; }

        /// The draw order within the screen, the screen's priority in the top 8 bits.
        /// The screen assigns it depth-first, so a child draws over
        /// its parent and a later sibling over an earlier one.
        int drawOrder() const { return _drawOrder; }
        void setDrawOrder(int value);
        bool textDirty() const { return _textDirty; }
        void clearTextDirty() { _textDirty = false; }

    protected:
        // GraphNodeTransformHook.
        void syncTransform(GraphNode& node) override;
        void setNodePosition(GraphNode& node, const Vector3& position) override;
        void setNodeLocalPosition(GraphNode& node, const Vector3& position) override;

    private:
        float absLeft() const { return _localAnchor.getX() + _margin.getX(); }
        float absRight() const { return _localAnchor.getZ() - _margin.getZ(); }
        float absTop() const { return _localAnchor.getW() - _margin.getW(); }
        float absBottom() const { return _localAnchor.getY() + _margin.getY(); }
        bool hasSplitAnchorsX() const;
        bool hasSplitAnchorsY() const;
        /// `value` within the sprite's frames; floored at 0 without a sprite or frames.
        int clampedSpriteFrame(int value) const;

        ElementComponent* parentElement() const;
        void calculateLocalAnchors();
        void calculateSize(bool propagateCalculatedWidth, bool propagateCalculatedHeight);
        void setWidthInternal(float w);
        void setHeightInternal(float h);
        void setCalculatedWidthInternal(float value, bool updateMargins);
        void setCalculatedHeightInternal(float value, bool updateMargins);
        void updateMarginsFromPosition();
        void flagChildrenAsDirty();
        void dirtifyAllCorners();
        void onInsert();
        Entity* parseUpToScreen() const;
        void dirtifyLocal();
        /// Bring the entity's world transform up to date, which marks the corners dirty when it moved.
        void syncEntityTransform();
        /// A text input changed: mark the mesh stale and measure again.
        void textChanged();
        /// A colour, outline or shadow changed. Markup styles fall back to these, so a text
        /// with tags rebuilds its per-style parts; without tags only the uniforms change.
        void styleChanged()
        {
            if (!_markupTags.empty()) {
                _textDirty = true;
            }
        }
        void updateTextLayout();

        inline static ComponentInstanceList<ElementComponent> _instanceList;

        static inline uint64_t _nextSerial = 1;
        uint64_t _serial = _nextSerial++;
        ElementType _type = ElementType::Group;

        Vector4 _anchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector4 _localAnchor = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
        Vector2 _pivot = Vector2(0.0f, 0.0f);
        float _width = 32.0f;
        float _height = 32.0f;
        float _calculatedWidth = 32.0f;
        float _calculatedHeight = 32.0f;
        Vector4 _margin = Vector4(0.0f, 0.0f, -32.0f, -32.0f);

        Matrix4 _modelTransform = Matrix4::identity();
        Matrix4 _screenToWorld = Matrix4::identity();
        Matrix4 _anchorTransform = Matrix4::identity();
        Matrix4 _parentWorldTransform = Matrix4::identity();
        Matrix4 _screenTransform = Matrix4::identity();
        bool _anchorDirty = true;
        bool _sizeDirty = false;

        std::array<Vector3, 4> _screenCorners{};
        std::array<Vector2, 4> _canvasCorners{};
        std::array<Vector3, 4> _worldCorners{};
        bool _cornersDirty = true;
        bool _canvasCornersDirty = true;
        bool _worldCornersDirty = true;

        Entity* _screen = nullptr;
        EventHandlePtr _onInsertHandle;
        // Localization (the key and its three i18n subscriptions), subscribed the
        // first time a key is set: a component added to a live entity gets no onEnable here.
        void subscribeLocalization();
        void resetLocalizedText();
        std::string _i18nKey;
        EventHandlePtr _localeHandle;
        EventHandlePtr _localeDataAddHandle;
        EventHandlePtr _localeDataRemoveHandle;

        float _opacity = 1.0f;
        Color _color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        int _fontSize = 32;
        std::string _text;
        FontResource* _fontResource = nullptr;
        ElementHorizontalAlign _horizontalAlign = ElementHorizontalAlign::Center;
        bool _wrapLines = false;
        float _verticalAlign = 0.5f;
        std::optional<float> _lineHeight;
        int _fittedFontSize = 32;
        int _minFontSize = 8;
        int _maxFontSize = 32;
        bool _autoFitWidth = false;
        bool _autoFitHeight = false;
        int _maxLines = -1;
        float _spacing = 1.0f;
        bool _enableMarkup = false;
        bool _justify = false;
        int _rangeStart = 0;
        int _rangeEnd = 0;
        uint64_t _rangeVersion = 1;
        bool _autoWidth = true;
        bool _autoHeight = true;
        std::string _symbols;
        std::u32string _codePoints;
        std::vector<std::optional<MarkupTags>> _markupTags;
        float _textWidth = 0.0f;
        float _textHeight = 0.0f;
        bool _inTextLayout = false;
        Color _outlineColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        float _outlineThickness = 0.0f;
        Color _shadowColor = Color(0.0f, 0.0f, 0.0f, 1.0f);
        Vector2 _shadowOffset = Vector2(0.0f, 0.0f);
        bool _textDirty = true;
        bool _useInput = false;
        std::vector<int> _layers;

        Texture* _texture = nullptr;
        std::shared_ptr<Sprite> _sprite;
        int _spriteFrame = 0;
        Vector4 _rect = Vector4(0.0f, 0.0f, 1.0f, 1.0f);
        std::optional<float> _pixelsPerUnit;
        ElementFitMode _fitMode = ElementFitMode::Stretch;
        uint64_t _imageVersion = 1;
        bool _mask = false;
        std::shared_ptr<Material> _customMaterial;
        ElementComponent* _maskedBy = nullptr;
        const void* _drawRecordOwner = nullptr;
        void* _drawRecord = nullptr;
        // Set once some element's _maskedBy has pointed here; the destructor then clears
        // those pointers (see ~ElementComponent).
        bool _wasUsedAsMask = false;
        int _drawOrder = 0;
    };
}
