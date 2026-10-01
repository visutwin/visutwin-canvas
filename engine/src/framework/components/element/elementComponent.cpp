// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Upstream element/component.js and the layout half of element/system.js. Method names
// follow upstream's (`_calculateSize` is calculateSize, `_sync` is syncTransform) so the two
// can be read side by side; the arithmetic and its ORDER are upstream's, because several
// setters feed each other through the entity's position.
//
#include "elementComponent.h"

#include <algorithm>
#include <cmath>

#include "core/math/quaternion.h"
#include "framework/components/screen/screenComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "framework/i18n/i18n.h"
#include "framework/components/element/textLayout.h"
#include "scene/sprite.h"

#include <limits>

#include <spdlog/spdlog.h>

namespace visutwin::canvas
{
    namespace
    {
        Matrix4 translate(const float x, const float y, const float z)
        {
            return Matrix4::translation(x, y, z);
        }
    }

    ElementComponent::ElementComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instanceList.add(this);
        if (_entity) {
            // Upstream constructor: `entity.on('insert', ...)` and `_patch()`.
            _onInsertHandle = _entity->on("insert", [this](GraphNode* /*parent*/) { onInsert(); });
            _entity->setTransformHook(this);
        }
    }

    ElementComponent::~ElementComponent()
    {
        _instanceList.remove(this);
        // An element masked by this one must not keep a pointer to it until the next frame
        // works the masks out again: a hit test in between would follow it. Only an element
        // that was ever handed out as a mask can be pointed at, so every other element's
        // destructor skips the sweep; running it for each would make destroying N
        // elements cost N x N.
        if (_wasUsedAsMask) {
            _instanceList.forEachLive([this](ElementComponent* element) {
                if (element->_maskedBy == this) {
                    element->_maskedBy = nullptr;
                }
            });
        }
        if (_onInsertHandle) {
            _onInsertHandle->off();
        }
        for (const auto& handle : {_localeHandle, _localeDataAddHandle, _localeDataRemoveHandle}) {
            if (handle) {
                handle->off();
            }
        }
        if (_entity && _entity->transformHook() == this) {
            _entity->setTransformHook(nullptr);   // upstream `_unpatch`
        }
        if (auto* screen = screenComponent()) {
            screen->unbindElement(this);
            screen->syncDrawOrder();
        }
    }

    void ElementComponent::initializeComponentData()
    {
        setup(ElementDesc{});
    }

    void ElementComponent::setup(const ElementDesc& desc)
    {
        // Upstream ElementComponentSystem.initializeComponentData, layout part.
        if (desc.anchor) {
            _anchor = *desc.anchor;
        }
        if (desc.pivot) {
            _pivot = *desc.pivot;
        }

        const bool splitHorAnchors = hasSplitAnchorsX();
        const bool splitVerAnchors = hasSplitAnchorsY();

        // On an axis where the anchor is a point, the entity's position places the element,
        // so unless margins are given for that axis, derive them from the position, as
        // setting a new size does. Do it FIRST (upstream #9525): applying the margins given
        // for the other axis, or binding to a screen, places the element on both axes and
        // would move an entity already under a screen to wherever the default margins put it.
        const Vector3 position = _entity ? _entity->localPosition() : Vector3(0.0f, 0.0f, 0.0f);
        const bool noMargin = !desc.margin;
        float mx = _margin.getX();
        float my = _margin.getY();
        float mz = _margin.getZ();
        float mw = _margin.getW();
        if (!splitHorAnchors && noMargin && !desc.left && !desc.right) {
            mx = position.getX() - _calculatedWidth * _pivot.x;
            mz = -_calculatedWidth - mx;
        }
        if (!splitVerAnchors && noMargin && !desc.bottom && !desc.top) {
            my = position.getY() - _calculatedHeight * _pivot.y;
            mw = -_calculatedHeight - my;
        }
        _margin = Vector4(mx, my, mz, mw);

        bool marginChange = false;
        if (desc.margin) {
            _margin = *desc.margin;
            marginChange = true;
        }
        mx = _margin.getX();
        my = _margin.getY();
        mz = _margin.getZ();
        mw = _margin.getW();
        if (desc.left) { mx = *desc.left; marginChange = true; }
        if (desc.bottom) { my = *desc.bottom; marginChange = true; }
        if (desc.right) { mz = *desc.right; marginChange = true; }
        if (desc.top) { mw = *desc.top; marginChange = true; }
        _margin = Vector4(mx, my, mz, mw);
        if (marginChange) {
            setMargin(_margin);   // force update
        }

        bool shouldForceSetAnchor = false;
        if (desc.width && !splitHorAnchors) {
            setWidth(*desc.width);
        } else if (splitHorAnchors) {
            shouldForceSetAnchor = true;
        }
        if (desc.height && !splitVerAnchors) {
            setHeight(*desc.height);
        } else if (splitVerAnchors) {
            shouldForceSetAnchor = true;
        }
        if (shouldForceSetAnchor) {
            setAnchor(_anchor);   // force update
        }

        if (desc.useInput) {
            _useInput = *desc.useInput;
        }
        if (desc.type) {
            setType(*desc.type);
        }

        // Find the screen here, not in the constructor, once the component is on the entity.
        if (Entity* screen = parseUpToScreen()) {
            updateScreen(screen);
        }
    }

    ScreenComponent* ElementComponent::screenComponent() const
    {
        return _screen ? _screen->findComponent<ScreenComponent>() : nullptr;
    }

    bool ElementComponent::hasSplitAnchorsX() const
    {
        return std::abs(_anchor.getX() - _anchor.getZ()) > 0.001f;
    }

    bool ElementComponent::hasSplitAnchorsY() const
    {
        return std::abs(_anchor.getY() - _anchor.getW()) > 0.001f;
    }

    ElementComponent* ElementComponent::parentElement() const
    {
        auto* parent = _entity ? dynamic_cast<Entity*>(_entity->parent()) : nullptr;
        return parent ? parent->findComponent<ElementComponent>() : nullptr;
    }

    void ElementComponent::dirtifyLocal()
    {
        if (_entity) {
            GraphNodeTransformHook::dirtifyLocal(*_entity);
        }
    }

    // ---- property setters ------------------------------------------------------------

    void ElementComponent::setAnchor(const Vector4& value)
    {
        _anchor = value;
        if ((!_entity || !_entity->parent()) && !_screen) {
            calculateLocalAnchors();
        } else {
            calculateSize(hasSplitAnchorsX(), hasSplitAnchorsY());
        }
        _anchorDirty = true;
        dirtifyLocal();
        fire("set:anchor", _anchor);
    }

    void ElementComponent::setPivot(const Vector2& value)
    {
        const float prevX = _pivot.x;
        const float prevY = _pivot.y;
        _pivot = value;

        // Keep the element where it is: move the margins by the pivot's travel.
        const float mxSum = _margin.getX() + _margin.getZ();
        const float dx = _pivot.x - prevX;
        const float mySum = _margin.getY() + _margin.getW();
        const float dy = _pivot.y - prevY;
        _margin = Vector4(_margin.getX() + mxSum * dx, _margin.getY() + mySum * dy,
            _margin.getZ() - mxSum * dx, _margin.getW() - mySum * dy);

        _anchorDirty = true;
        dirtifyAllCorners();
        calculateSize(false, false);
        flagChildrenAsDirty();
        fire("set:pivot", _pivot);
    }

    void ElementComponent::setMargin(const Vector4& value)
    {
        _margin = value;
        calculateSize(true, true);
        fire("set:margin", _margin);
    }

    void ElementComponent::setLeft(const float value)
    {
        _margin = Vector4(value, _margin.getY(), _margin.getZ(), _margin.getW());
        Vector3 p = _entity ? _entity->localPosition() : Vector3(0.0f, 0.0f, 0.0f);
        const float wr = absRight();
        const float wl = _localAnchor.getX() + value;
        setWidthInternal(wr - wl);
        p = Vector3(value + _calculatedWidth * _pivot.x, p.getY(), p.getZ());
        if (_entity) {
            _entity->setLocalPosition(p);
        }
    }

    void ElementComponent::setRight(const float value)
    {
        _margin = Vector4(_margin.getX(), _margin.getY(), value, _margin.getW());
        Vector3 p = _entity ? _entity->localPosition() : Vector3(0.0f, 0.0f, 0.0f);
        const float wl = absLeft();
        const float wr = _localAnchor.getZ() - value;
        setWidthInternal(wr - wl);
        p = Vector3(_localAnchor.getZ() - _localAnchor.getX() - value - _calculatedWidth * (1.0f - _pivot.x),
            p.getY(), p.getZ());
        if (_entity) {
            _entity->setLocalPosition(p);
        }
    }

    void ElementComponent::setBottom(const float value)
    {
        _margin = Vector4(_margin.getX(), value, _margin.getZ(), _margin.getW());
        Vector3 p = _entity ? _entity->localPosition() : Vector3(0.0f, 0.0f, 0.0f);
        const float wt = absTop();
        const float wb = _localAnchor.getY() + value;
        setHeightInternal(wt - wb);
        p = Vector3(p.getX(), value + _calculatedHeight * _pivot.y, p.getZ());
        if (_entity) {
            _entity->setLocalPosition(p);
        }
    }

    void ElementComponent::setTop(const float value)
    {
        _margin = Vector4(_margin.getX(), _margin.getY(), _margin.getZ(), value);
        Vector3 p = _entity ? _entity->localPosition() : Vector3(0.0f, 0.0f, 0.0f);
        const float wb = absBottom();
        const float wt = _localAnchor.getW() - value;
        setHeightInternal(wt - wb);
        p = Vector3(p.getX(),
            _localAnchor.getW() - _localAnchor.getY() - value - _calculatedHeight * (1.0f - _pivot.y), p.getZ());
        if (_entity) {
            _entity->setLocalPosition(p);
        }
    }

    void ElementComponent::setWidth(const float value)
    {
        _width = value;
        if (!hasSplitAnchorsX()) {
            setCalculatedWidthInternal(value, true);
        }
        fire("set:width", _width);
        _textDirty = true;
    }

    void ElementComponent::setHeight(const float value)
    {
        _height = value;
        if (!hasSplitAnchorsY()) {
            setCalculatedHeightInternal(value, true);
        }
        fire("set:height", _height);
        _textDirty = true;
    }

    // ---- size ------------------------------------------------------------------------

    void ElementComponent::calculateLocalAnchors()
    {
        // Pixel positions of the anchors in the current parent rectangle.
        float resx = 1000.0f;
        float resy = 1000.0f;
        if (auto* parent = parentElement()) {
            resx = parent->_calculatedWidth;
            resy = parent->_calculatedHeight;
        } else if (auto* screen = screenComponent()) {
            resx = screen->resolution().x / screen->scale();
            resy = screen->resolution().y / screen->scale();
        }
        _localAnchor = Vector4(_anchor.getX() * resx, _anchor.getY() * resy,
            _anchor.getZ() * resx, _anchor.getW() * resy);
    }

    void ElementComponent::calculateSize(const bool propagateCalculatedWidth, const bool propagateCalculatedHeight)
    {
        // Cannot calculate while the local anchors are meaningless.
        if ((!_entity || !_entity->parent()) && !_screen) {
            return;
        }

        calculateLocalAnchors();

        const float newWidth = absRight() - absLeft();
        const float newHeight = absTop() - absBottom();

        if (propagateCalculatedWidth) {
            setWidthInternal(newWidth);
        } else {
            setCalculatedWidthInternal(newWidth, false);
        }
        if (propagateCalculatedHeight) {
            setHeightInternal(newHeight);
        } else {
            setCalculatedHeightInternal(newHeight, false);
        }

        const Vector3 p = _entity->localPosition();
        _entity->setLocalPosition(Vector3(_margin.getX() + _calculatedWidth * _pivot.x,
            _margin.getY() + _calculatedHeight * _pivot.y, p.getZ()));

        _sizeDirty = false;
    }

    void ElementComponent::setWidthInternal(const float w)
    {
        _width = w;
        setCalculatedWidthInternal(w, false);
        fire("set:width", _width);
        _textDirty = true;
    }

    void ElementComponent::setHeightInternal(const float h)
    {
        _height = h;
        setCalculatedHeightInternal(h, false);
        fire("set:height", _height);
        _textDirty = true;
    }

    void ElementComponent::setCalculatedWidthInternal(const float value, const bool updateMargins)
    {
        if (std::abs(value - _calculatedWidth) <= 1e-4f) {
            return;
        }
        _calculatedWidth = value;
        dirtifyLocal();
        if (updateMargins && _entity) {
            const float px = _entity->localPosition().getX();
            const float mx = px - _calculatedWidth * _pivot.x;
            const float mz = _localAnchor.getZ() - _localAnchor.getX() - _calculatedWidth - mx;
            _margin = Vector4(mx, _margin.getY(), mz, _margin.getW());
        }
        flagChildrenAsDirty();
        fire("set:calculatedWidth", _calculatedWidth);
        fire("resize", _calculatedWidth, _calculatedHeight);
        // Wrapped text breaks at this width, and auto-fitted text fits it, so either follows it.
        if (_type == ElementType::Text &&
            ((_wrapLines && std::isfinite(textMaxLineWidth())) || shouldAutoFitWidth())) {
            updateTextLayout();
        }
    }

    void ElementComponent::setCalculatedHeightInternal(const float value, const bool updateMargins)
    {
        if (std::abs(value - _calculatedHeight) <= 1e-4f) {
            return;
        }
        _calculatedHeight = value;
        dirtifyLocal();
        if (updateMargins && _entity) {
            const float py = _entity->localPosition().getY();
            const float my = py - _calculatedHeight * _pivot.y;
            const float mw = _localAnchor.getW() - _localAnchor.getY() - _calculatedHeight - my;
            _margin = Vector4(_margin.getX(), my, _margin.getZ(), mw);
        }
        flagChildrenAsDirty();
        fire("set:calculatedHeight", _calculatedHeight);
        fire("resize", _calculatedWidth, _calculatedHeight);
        // Auto-fitted text fits the height, so it follows it.
        if (_type == ElementType::Text && shouldAutoFitHeight()) {
            updateTextLayout();
        }
    }

    void ElementComponent::updateMarginsFromPosition()
    {
        const Vector3& p = _entity->localPosition();
        const float mx = p.getX() - _calculatedWidth * _pivot.x;
        const float mz = _localAnchor.getZ() - _localAnchor.getX() - _calculatedWidth - mx;
        const float my = p.getY() - _calculatedHeight * _pivot.y;
        const float mw = _localAnchor.getW() - _localAnchor.getY() - _calculatedHeight - my;
        _margin = Vector4(mx, my, mz, mw);
    }

    void ElementComponent::flagChildrenAsDirty()
    {
        if (!_entity) {
            return;
        }
        for (const auto& child : _entity->children()) {
            auto* entity = dynamic_cast<Entity*>(child.get());
            if (auto* element = entity ? entity->findComponent<ElementComponent>() : nullptr) {
                element->_anchorDirty = true;
                element->_sizeDirty = true;
            }
        }
    }

    void ElementComponent::dirtifyAllCorners()
    {
        _cornersDirty = true;
        _canvasCornersDirty = true;
        _worldCornersDirty = true;
    }

    // ---- screen ----------------------------------------------------------------------

    Entity* ElementComponent::parseUpToScreen() const
    {
        GraphNode* parent = _entity ? _entity->parent() : nullptr;
        while (parent) {
            auto* entity = dynamic_cast<Entity*>(parent);
            if (entity && entity->findComponent<ScreenComponent>()) {
                return entity;
            }
            parent = parent->parent();
        }
        return nullptr;
    }

    void ElementComponent::onInsert()
    {
        // Reparented: find a possible new screen.
        if (_entity) {
            GraphNodeTransformHook::dirtifyWorld(*_entity);
        }
        updateScreen(parseUpToScreen());
    }

    void ElementComponent::updateScreen(Entity* screen)
    {
        if (_screen && _screen != screen) {
            if (auto* old = screenComponent()) {
                old->unbindElement(this);
                old->syncDrawOrder();
            }
        }

        _screen = screen;
        if (auto* current = screenComponent()) {
            current->bindElement(this);
            current->syncDrawOrder();   // upstream: every (re)bind re-derives the order
        }

        calculateSize(hasSplitAnchorsX(), hasSplitAnchorsY());
        fire("set:screen", _screen);
        _anchorDirty = true;

        // Update every child element's screen.
        if (_entity) {
            for (const auto& child : _entity->children()) {
                auto* entity = dynamic_cast<Entity*>(child.get());
                if (auto* element = entity ? entity->findComponent<ElementComponent>() : nullptr) {
                    element->updateScreen(screen);
                }
            }
        }
    }

    void ElementComponent::onScreenResize(const Vector2& resolution)
    {
        _anchorDirty = true;
        _cornersDirty = true;
        _worldCornersDirty = true;
        calculateSize(hasSplitAnchorsX(), hasSplitAnchorsY());
        fire("screen:set:resolution", resolution);
    }

    void ElementComponent::onScreenRemove(ScreenComponent* screen)
    {
        // The screen component is going away: a stale pointer must not survive it
        // (upstream #1151). Its entity may be going too, so nothing is recomputed against it.
        // Compared by ENTITY: by the time a screen's destructor runs, its entity no longer
        // returns it from findComponent, so screenComponent() cannot recognise it.
        if (_screen && screen && _screen == screen->entity()) {
            _screen = nullptr;
            _anchorDirty = true;
            dirtifyLocal();
        }
    }

    // ---- transform hook (upstream _sync / _setPosition / _setLocalPosition) --------------

    void ElementComponent::setNodePosition(GraphNode& node, const Vector3& position)
    {
        if (!_screen) {
            defaultSetPosition(node, position);
            return;
        }
        node.worldTransform();   // ensure the hierarchy is up to date
        localPositionStorage(node) = _screenToWorld.inverse().transformPoint(position);
        if (!dirtyLocal(node)) {
            GraphNodeTransformHook::dirtifyLocal(node);
        }
    }

    void ElementComponent::setNodeLocalPosition(GraphNode& node, const Vector3& position)
    {
        localPositionStorage(node) = position;
        updateMarginsFromPosition();
        if (!dirtyLocal(node)) {
            GraphNodeTransformHook::dirtifyLocal(node);
        }
    }

    void ElementComponent::syncTransform(GraphNode& node)
    {
        ScreenComponent* screen = screenComponent();
        ElementComponent* parent = parentElement();

        if (screen) {
            if (_anchorDirty) {
                float resx = 0.0f;
                float resy = 0.0f;
                float px = 0.0f;
                float py = 1.0f;
                if (parent) {
                    // The parent element's rectangle.
                    resx = parent->_calculatedWidth;
                    resy = parent->_calculatedHeight;
                    px = parent->_pivot.x;
                    py = parent->_pivot.y;
                } else {
                    // The screen's.
                    resx = screen->resolution().x / screen->scale();
                    resy = screen->resolution().y / screen->scale();
                }
                _anchorTransform = translate(resx * (_anchor.getX() - px), -(resy * (py - _anchor.getY())), 0.0f);
                _anchorDirty = false;
                calculateLocalAnchors();
            }

            // Order matters: calculateSize dirtifies the local transform, so it runs
            // before that is cleared below.
            if (_sizeDirty) {
                calculateSize(false, false);
            }
        }

        if (dirtyLocal(node)) {
            localTransform(node) = Matrix4::trs(node.localPosition(), node.localRotation(), node.localScale());
            updateMarginsFromPosition();
            dirtyLocal(node) = false;
        }

        if (!screen) {
            if (dirtyWorld(node)) {
                dirtifyAllCorners();
            }
            defaultSync(node);
            return;
        }

        if (!dirtyWorld(node)) {
            return;
        }

        if (node.parent() == nullptr) {
            worldTransformStorage(node) = localTransform(node);
        } else {
            // The element hierarchy.
            _screenToWorld = parent ? parent->_modelTransform * _anchorTransform : _anchorTransform;
            _modelTransform = _screenToWorld * localTransform(node);

            _screenToWorld = screen->screenMatrix() * _screenToWorld;
            if (!screen->screenSpace()) {
                _screenToWorld = _screen->worldTransform() * _screenToWorld;
            }
            worldTransformStorage(node) = _screenToWorld * localTransform(node);

            // The parent's world transform, as the corners need it.
            _parentWorldTransform = Matrix4::identity();
            auto* parentEntity = dynamic_cast<Entity*>(node.parent());
            if (parent && parentEntity != _screen) {
                const Matrix4 parentRotScale = Matrix4::trs(Vector3(0.0f, 0.0f, 0.0f),
                    parentEntity->localRotation(), parentEntity->localScale());
                _parentWorldTransform = parent->_parentWorldTransform * parentRotScale;
            }

            // Rotate and scale around the pivot.
            const Vector3 depthOffset(0.0f, 0.0f, node.localPosition().getZ());
            const Vector3 pivotOffset(absLeft() + _pivot.x * _calculatedWidth,
                absBottom() + _pivot.y * _calculatedHeight, 0.0f);
            const Matrix4 toPivot = translate(-pivotOffset.getX(), -pivotOffset.getY(), -pivotOffset.getZ());
            const Matrix4 rotScale = Matrix4::trs(depthOffset, node.localRotation(), node.localScale());
            const Matrix4 fromPivot = translate(pivotOffset.getX(), pivotOffset.getY(), pivotOffset.getZ());
            _screenTransform = _parentWorldTransform * fromPivot * rotScale * toPivot;

            dirtifyAllCorners();
        }

        dirtyWorld(node) = false;
    }

    // ---- corners ---------------------------------------------------------------------

    void ElementComponent::syncEntityTransform()
    {
        if (_entity) {
            _entity->worldTransform();
        }
    }

    const std::array<Vector3, 4>& ElementComponent::screenCorners()
    {
        // Sync FIRST: transforms here are lazy, and the sync is what marks the corners dirty.
        // Upstream syncs the whole hierarchy every frame, so its corners are at most a frame old;
        // here they would stay where the element was until something asked for a transform.
        // This also brings _screenTransform up to date.
        syncEntityTransform();
        ScreenComponent* screen = screenComponent();
        if (!_cornersDirty || !screen) {
            return _screenCorners;
        }

        const ElementComponent* parent = parentElement();
        const std::optional<Vector3> parentBottomLeft = parent
            ? std::optional<Vector3>(const_cast<ElementComponent*>(parent)->screenCorners()[0]) : std::nullopt;

        const float left = absLeft();
        const float bottom = absBottom();
        const float right = absRight();
        const float top = absTop();
        _screenCorners[0] = Vector3(left, bottom, 0.0f);
        _screenCorners[1] = Vector3(right, bottom, 0.0f);
        _screenCorners[2] = Vector3(right, top, 0.0f);
        _screenCorners[3] = Vector3(left, top, 0.0f);

        const bool screenSpace = screen->screenSpace();
        for (auto& corner : _screenCorners) {
            corner = _screenTransform.transformPoint(corner);
            if (screenSpace) {
                corner = corner * screen->scale();
            }
            if (parentBottomLeft) {
                corner = corner + *parentBottomLeft;
            }
        }

        _cornersDirty = false;
        _canvasCornersDirty = true;
        _worldCornersDirty = true;
        return _screenCorners;
    }

    const std::array<Vector2, 4>& ElementComponent::canvasCorners()
    {
        syncEntityTransform();
        ScreenComponent* screen = screenComponent();
        if (!_canvasCornersDirty || !screen || !screen->screenSpace()) {
            return _canvasCorners;
        }
        // The screen's resolution IS the canvas size in points for a screen-space screen, so
        // upstream's clientWidth / width ratio is 1 here; only y turns over.
        const auto& corners = screenCorners();
        const float height = screen->resolution().y;
        for (size_t i = 0; i < 4; ++i) {
            _canvasCorners[i] = Vector2(corners[i].getX(), height - corners[i].getY());
        }
        _canvasCornersDirty = false;
        return _canvasCorners;
    }

    const std::array<Vector3, 4>& ElementComponent::worldCorners()
    {
        syncEntityTransform();
        if (!_worldCornersDirty) {
            return _worldCorners;
        }

        if (ScreenComponent* screen = screenComponent()) {
            const auto& corners = screenCorners();
            if (!screen->screenSpace()) {
                // The screen matrix flipped along the horizontal axis, into world space.
                Matrix4 m = screen->screenMatrix();
                m.setElement(3, 1, -m.getElement(3, 1));
                m = _screen->worldTransform() * m;
                for (size_t i = 0; i < 4; ++i) {
                    _worldCorners[i] = m.transformPoint(corners[i]);
                }
            }
        } else if (_entity) {
            const Vector3 localPos = _entity->localPosition();
            const Matrix4 toPos = translate(-localPos.getX(), -localPos.getY(), -localPos.getZ());
            const Matrix4 rotScale = Matrix4::trs(Vector3(0.0f, 0.0f, 0.0f), _entity->localRotation(), _entity->localScale());
            const Matrix4 fromPos = translate(localPos.getX(), localPos.getY(), localPos.getZ());
            // The parent's world transform, or this entity's when it has none.
            GraphNode* basis = _entity->parent() ? _entity->parent() : _entity;
            const Matrix4 m = basis->worldTransform() * fromPos * rotScale * toPos;

            const float w = _calculatedWidth;
            const float h = _calculatedHeight;
            const float x = localPos.getX();
            const float y = localPos.getY();
            const float z = localPos.getZ();
            _worldCorners[0] = m.transformPoint(Vector3(x - _pivot.x * w, y - _pivot.y * h, z));
            _worldCorners[1] = m.transformPoint(Vector3(x + (1.0f - _pivot.x) * w, y - _pivot.y * h, z));
            _worldCorners[2] = m.transformPoint(Vector3(x + (1.0f - _pivot.x) * w, y + (1.0f - _pivot.y) * h, z));
            _worldCorners[3] = m.transformPoint(Vector3(x - _pivot.x * w, y + (1.0f - _pivot.y) * h, z));
        }

        _worldCornersDirty = false;
        return _worldCorners;
    }

    // ---- text ------------------------------------------------------------------------

    float ElementComponent::textMaxLineWidth() const
    {
        if ((_autoWidth && !hasSplitAnchorsX()) || !_wrapLines) {
            return std::numeric_limits<float>::infinity();
        }
        return _calculatedWidth;
    }

    void ElementComponent::setKey(const std::string& value)
    {
        if (_i18nKey == value) {
            return;
        }
        _i18nKey = value;
        if (!_i18nKey.empty()) {
            subscribeLocalization();
            resetLocalizedText();
        }
    }

    void ElementComponent::subscribeLocalization()
    {
        if (_localeHandle) {
            return;
        }
        I18n* i18n = _entity && _entity->engine() ? _entity->engine()->i18n() : nullptr;
        if (!i18n) {
            return;
        }
        // Upstream _onLocaleSet (less its font swap) and _onLocalizationData.
        _localeHandle = i18n->on("change", [this](const std::string& /*locale*/, const std::string& /*old*/) {
            if (!_i18nKey.empty()) {
                resetLocalizedText();
            }
        });
        const auto onData = [this](const std::string& /*locale*/, const std::vector<std::string>& keys) {
            if (!_i18nKey.empty() && std::find(keys.begin(), keys.end(), _i18nKey) != keys.end()) {
                resetLocalizedText();
            }
        };
        _localeDataAddHandle = i18n->on("data:add", onData);
        _localeDataRemoveHandle = i18n->on("data:remove", onData);
    }

    void ElementComponent::resetLocalizedText()
    {
        I18n* i18n = _entity && _entity->engine() ? _entity->engine()->i18n() : nullptr;
        _text = i18n ? i18n->getText(_i18nKey) : _i18nKey;
        textChanged();
    }

    void ElementComponent::textChanged()
    {
        _textDirty = true;
        updateTextLayout();
    }

    TextMeasure ElementComponent::measureLayout() const
    {
        if (!_fontResource) {
            return {};
        }
        const auto size = static_cast<float>(fontSize());
        const float step = shouldAutoFit()
            ? lineHeight() * size / std::max(static_cast<float>(_maxFontSize), 0.0001f) : lineHeight();
        return measureText(*_fontResource, _codePoints, size, step, textMaxLineWidth(), _spacing,
                           _wrapLines ? _maxLines : -1);
    }

    void ElementComponent::updateTextLayout()
    {
        // Upstream lays text out as soon as an input changes, so a caller can read the
        // element's size right after setting its text; so does this.
        if (_type != ElementType::Text || !_fontResource || _inTextLayout) {
            return;
        }
        _inTextLayout = true;
        _markupTags.clear();
        if (_enableMarkup) {
            MarkupResult markup = evaluateMarkup(_text);
            if (!markup.error.empty()) {
                spdlog::warn("{} in text '{}'", markup.error, _text);
            }
            _symbols = std::move(markup.symbols);
            _markupTags = std::move(markup.tags);
        } else {
            _symbols = _text;
        }
        _codePoints = decodeUtf8(_symbols);
        if (!_markupTags.empty() && _codePoints.size() != _symbols.size()) {
            // The markup parser tags BYTES; a code point takes the tag of its first byte.
            std::vector<std::optional<MarkupTags>> perCodePoint;
            perCodePoint.reserve(_codePoints.size());
            for (size_t i = 0; i < _symbols.size() && i < _markupTags.size(); ++i) {
                if ((static_cast<unsigned char>(_symbols[i]) & 0xC0) != 0x80) {
                    perCodePoint.push_back(std::move(_markupTags[i]));
                }
            }
            perCodePoint.resize(_codePoints.size());
            _markupTags = std::move(perCodePoint);
        }
        // A new layout draws the whole text again (upstream _updateText).
        _rangeStart = 0;
        _rangeEnd = static_cast<int>(_codePoints.size());
        ++_rangeVersion;
        // Upstream's auto fit (text-element.js _updateMeshes): start at maxFontSize and lay out
        // again smaller while the text overflows — a width overflow scales the size by how far
        // it overflows (floored), a height overflow takes it down by one — within
        // [minFontSize, maxFontSize]. Upstream tests the overflow glyph by glyph; the text's
        // width grows linearly with the size, so testing the whole measure lands on the same size.
        const int minFont = std::min(_minFontSize, _maxFontSize);
        const int maxFont = _maxFontSize;
        _fittedFontSize = shouldAutoFit() ? _maxFontSize : _fontSize;
        TextMeasure measure = measureLayout();
        while (shouldAutoFit()) {
            int size = _fittedFontSize;
            if (shouldAutoFitWidth() && measure.width > _calculatedWidth) {
                size = std::clamp(static_cast<int>(std::floor(static_cast<float>(_fittedFontSize) * _calculatedWidth /
                                                              std::max(measure.width, 0.0001f))), minFont, maxFont);
            }
            if (size == _fittedFontSize && shouldAutoFitHeight() && measure.height > _calculatedHeight) {
                size = std::clamp(_fittedFontSize - 1, minFont, maxFont);
            }
            if (size == _fittedFontSize) {
                break;
            }
            _fittedFontSize = size;
            measure = measureLayout();
        }
        _textWidth = measure.width;
        _textHeight = measure.height;
        // Upstream's autoWidth / autoHeight setters, run after every layout: the element
        // takes the text's size on an axis its anchors do not split.
        if (_autoWidth && !hasSplitAnchorsX()) {
            setWidth(_textWidth);
        }
        if (_autoHeight && !hasSplitAnchorsY()) {
            setHeight(_textHeight);
        }
        _inTextLayout = false;
    }

    // ---- image -----------------------------------------------------------------------

    void ElementComponent::setTexture(Texture* value)
    {
        _texture = value;
        if (value) {
            _sprite.reset();   // upstream: a texture clears the sprite
        }
        ++_imageVersion;
    }

    void ElementComponent::setSprite(std::shared_ptr<Sprite> value)
    {
        const bool changed = value != _sprite;
        _sprite = std::move(value);
        if (_sprite) {
            _texture = nullptr;   // and a sprite clears the texture
        }
        ++_imageVersion;
        if (changed) {
            fire("set:sprite");
        }
    }

    void ElementComponent::setSpriteFrame(const int value)
    {
        const int frame = std::max(value, 0);
        if (frame == _spriteFrame) {
            return;
        }
        _spriteFrame = frame;
        ++_imageVersion;
        fire("set:spriteFrame");
    }

    void ElementComponent::setRangeStart(const int value)
    {
        const int start = std::clamp(value, 0, static_cast<int>(_codePoints.size()));
        if (start != _rangeStart) {
            _rangeStart = start;
            ++_rangeVersion;
        }
    }

    void ElementComponent::setRangeEnd(const int value)
    {
        const int end = std::clamp(value, _rangeStart, static_cast<int>(_codePoints.size()));
        if (end != _rangeEnd) {
            _rangeEnd = end;
            ++_rangeVersion;
        }
    }

    void ElementComponent::setOpacity(const float value)
    {
        const float opacity = std::clamp(value, 0.0f, 1.0f);
        if (opacity == _opacity) {
            return;
        }
        _opacity = opacity;
        fire("set:opacity");
    }

    void ElementComponent::setColor(const Color& value)
    {
        if (value == _color) {
            return;
        }
        _color = value;
        styleChanged();
        fire("set:color");
    }

    void ElementComponent::setDrawOrder(int value)
    {
        // Upstream: the screen's priority lives in the top 8 bits, the order in the rest.
        const int priority = screenComponent() ? screenComponent()->priority() : 0;
        value = std::clamp(value, 0, 0xFFFFFF);
        _drawOrder = (priority << 24) + value;
    }

    // ---- clone -----------------------------------------------------------------------

    void ElementComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const ElementComponent*>(source);
        if (!src) {
            return;
        }
        // Upstream cloneComponent passes width, height, anchor, pivot and margin through
        // initializeComponentData.
        ElementDesc desc;
        desc.type = src->_type;
        desc.width = src->_width;
        desc.height = src->_height;
        desc.anchor = src->_anchor;
        desc.pivot = src->_pivot;
        desc.margin = src->_margin;
        desc.useInput = src->_useInput;
        setup(desc);

        _opacity = src->_opacity;
        _color = src->_color;
        _fontSize = src->_fontSize;
        _fittedFontSize = src->_fittedFontSize;
        _minFontSize = src->_minFontSize;
        _maxFontSize = src->_maxFontSize;
        _autoFitWidth = src->_autoFitWidth;
        _autoFitHeight = src->_autoFitHeight;
        _maxLines = src->_maxLines;
        _text = src->_text;
        _fontResource = src->_fontResource;
        _horizontalAlign = src->_horizontalAlign;
        _wrapLines = src->_wrapLines;
        _verticalAlign = src->_verticalAlign;
        _lineHeight = src->_lineHeight;
        _spacing = src->_spacing;
        _enableMarkup = src->_enableMarkup;
        _justify = src->_justify;
        _mask = src->_mask;
        _customMaterial = src->_customMaterial;
        _autoWidth = src->_autoWidth;
        _autoHeight = src->_autoHeight;
        _outlineColor = src->_outlineColor;
        _outlineThickness = src->_outlineThickness;
        _shadowColor = src->_shadowColor;
        _shadowOffset = src->_shadowOffset;
        _layers = src->_layers;
        _texture = src->_texture;
        _sprite = src->_sprite;   // shared, as upstream's clone shares the sprite asset
        _spriteFrame = src->_spriteFrame;
        _rect = src->_rect;
        _pixelsPerUnit = src->_pixelsPerUnit;
        _fitMode = src->_fitMode;
        ++_imageVersion;
        if (!src->_i18nKey.empty()) {
            _i18nKey = src->_i18nKey;
            subscribeLocalization();
        }
        textChanged();   // the clone has no text mesh of its own yet
    }
}
