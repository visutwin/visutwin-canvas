// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// A UI element (upstream framework/components/element/component.js), its LAYOUT: an
// element sits in its parent element's rectangle, or its screen's, by ANCHORS (fractions
// of that rectangle, x/y the bottom-left corner and z/w the top-right), a PIVOT (the point
// of the element its position names, 0..1) and MARGINS (distances from the anchors: left,
// bottom, right, top). On an axis whose two anchors coincide the element keeps its own
// width or height and its position places it; on a "split" axis it stretches between the
// anchors and the margins set its size.
//
// Its entity's transform is computed by the element while it has a screen
// (GraphNodeTransformHook, upstream's patched `_sync`): the world transform goes through
// the anchors, the parent element's model transform and the screen's projection, so a
// SCREEN-SPACE element's world transform is in clip space and a camera that draws it maps
// world XY straight to NDC; a world-space screen's elements are ordinary world geometry.
// Setting the entity's position re-derives the margins, as upstream.
//
// Configure an element either with setters, each of which behaves like upstream's, or all
// at once with `setup(ElementDesc)`, which is upstream's `addComponent('element', data)`
// and applies the values in its order.
//
// Not ported yet: image and text element rendering beyond what ElementInput draws, masks,
// draw order, fit modes, layout groups and batching.
//
#pragma once

#include <algorithm>
#include <array>
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
#include "framework/handlers/fontResource.h"
#include "scene/graphNodeTransformHook.h"

namespace visutwin::canvas
{
    class ScreenComponent;

    /// Upstream ELEMENTTYPE_GROUP / IMAGE / TEXT. A group lays out and draws nothing.
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

    /// Upstream's `addComponent('element', data)`: the fields given, applied in its order.
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

        /// Upstream ElementComponentSystem.initializeComponentData for `desc`.
        void setup(const ElementDesc& desc);

        static const std::vector<ElementComponent*>& instances() { return _instances; }

        ElementType type() const { return _type; }
        void setType(ElementType value) { _type = value; _textDirty = true; }

        // ---- layout (upstream semantics) --------------------------------------------

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

        // ---- screen callbacks (upstream `_updateScreen`, `_onScreenResize`, ...) --------

        void updateScreen(Entity* screen);
        void onScreenResize(const Vector2& resolution);
        void onScreenRemove(ScreenComponent* screen);

        // ---- appearance, read by ElementInput ------------------------------------------

        float opacity() const { return _opacity; }
        void setOpacity(const float value) { _opacity = std::clamp(value, 0.0f, 1.0f); }
        const Color& color() const { return _color; }
        void setColor(const Color& value) { _color = value; }
        int fontSize() const { return _fontSize; }
        void setFontSize(const int value) { _fontSize = std::max(value, 1); _textDirty = true; }
        const std::string& text() const { return _text; }
        void setText(const std::string& value) { _text = value; _textDirty = true; }
        FontResource* fontResource() const { return _fontResource; }
        void setFontResource(FontResource* value) { _fontResource = value; _textDirty = true; }
        ElementHorizontalAlign horizontalAlign() const { return _horizontalAlign; }
        void setHorizontalAlign(const ElementHorizontalAlign value) { _horizontalAlign = value; _textDirty = true; }
        /// Where the block of lines sits vertically in the box: 0 bottom, 1 top (upstream
        /// `alignment.y`). DEVIATION: the default is 1, the top, where upstream centres
        /// (0.5); every port so far was laid out against the top.
        float verticalAlign() const { return _verticalAlign; }
        void setVerticalAlign(const float value) { _verticalAlign = std::clamp(value, 0.0f, 1.0f); _textDirty = true; }
        bool wrapLines() const { return _wrapLines; }
        void setWrapLines(const bool value) { _wrapLines = value; _textDirty = true; }
        /// The layers the element's visual is drawn on (upstream `layers`). Empty, the
        /// default, lets the element system choose: LAYERID_UI on a screen-space screen,
        /// LAYERID_WORLD otherwise. DEVIATION: upstream defaults to [LAYERID_UI] for both,
        /// which here would take world-space labels out of the scene camera's view.
        const std::vector<int>& layers() const { return _layers; }
        void setLayers(const std::vector<int>& value) { _layers = value; }
        bool useInput() const { return _useInput; }
        void setUseInput(const bool value) { _useInput = value; }
        bool textDirty() const { return _textDirty; }
        void clearTextDirty() { _textDirty = false; }

    protected:
        // GraphNodeTransformHook (upstream `_sync`, `_setPosition`, `_setLocalPosition`).
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

        inline static std::vector<ElementComponent*> _instances;

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

        float _opacity = 1.0f;
        Color _color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        int _fontSize = 16;
        std::string _text;
        FontResource* _fontResource = nullptr;
        ElementHorizontalAlign _horizontalAlign = ElementHorizontalAlign::Center;
        bool _wrapLines = false;
        float _verticalAlign = 1.0f;
        bool _textDirty = true;
        bool _useInput = false;
        std::vector<int> _layers;
    };
}
