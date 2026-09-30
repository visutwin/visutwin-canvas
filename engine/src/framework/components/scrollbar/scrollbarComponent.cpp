// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
#include "scrollbarComponent.h"

#include <algorithm>
#include <cmath>

#include "framework/components/element/elementComponent.h"
#include "framework/components/element/elementDragHelper.h"
#include "framework/entity.h"

namespace visutwin::canvas
{
    ScrollbarComponent::ScrollbarComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity)
    {
        _instances.push_back(this);
    }

    ScrollbarComponent::~ScrollbarComponent()
    {
        std::erase(_instances, this);
        // upstream onBeforeRemove: the elements may outlive the scrollbar.
        unbindTrackElement();
        unbindHandleElement();
        if (_handleEntityDestroyed) {
            _handleEntityDestroyed->off();
        }
    }

    void ScrollbarComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const ScrollbarComponent*>(source);
        if (!src) {
            return;
        }
        _orientation = src->_orientation;
        _value = src->_value;
        _handleSize = src->_handleSize;
        setHandleEntity(src->_handleEntity);
    }

    void ScrollbarComponent::resolveClonedReferences(const Component* source, const CloneNodeMap& map)
    {
        if (const auto* src = dynamic_cast<const ScrollbarComponent*>(source); src && src->_handleEntity) {
            setHandleEntity(remapCloned(src->_handleEntity, map));
        }
    }

    void ScrollbarComponent::onEnable()
    {
        if (_dragHelper) {
            _dragHelper->setEnabled(true);
        }
    }

    void ScrollbarComponent::onDisable()
    {
        if (_dragHelper) {
            _dragHelper->setEnabled(false);
        }
    }

    void ScrollbarComponent::setOrientation(const Orientation value)
    {
        if (_orientation == value) {
            return;
        }
        _orientation = value;
        // The drag helper captures its axis when it is built, so a new orientation needs a new
        // one; the handle loses its length on the axis it no longer runs along.
        if (_handleElement) {
            if (_orientation == Orientation::Horizontal) {
                _handleElement->setHeight(0.0f);
            } else {
                _handleElement->setWidth(0.0f);
            }
            rebuildDragHelper();
            updateHandlePositionAndSize();
        }
    }

    void ScrollbarComponent::setValue(const float value)
    {
        if (std::abs(value - _value) > 1e-5f) {
            _value = std::clamp(value, 0.0f, 1.0f);
            updateHandlePositionAndSize();
            fire("set:value", _value);
        }
    }

    void ScrollbarComponent::setHandleSize(const float value)
    {
        if (std::abs(value - _handleSize) > 1e-5f) {
            _handleSize = std::clamp(value, 0.0f, 1.0f);
            updateHandlePositionAndSize();
        }
    }

    void ScrollbarComponent::setHandleEntity(Entity* entity)
    {
        if (_handleEntity == entity) {
            return;
        }
        unbindHandleElement();
        if (_handleEntityDestroyed) {
            _handleEntityDestroyed->off();
            _handleEntityDestroyed.reset();
        }
        _handleEntity = entity;
        if (_handleEntity) {
            _handleEntityDestroyed = _handleEntity->on("destroy", [this]() {
                unbindHandleElement();
                _handleEntity = nullptr;
            });
        }
        refreshBindings();
    }

    void ScrollbarComponent::refreshBindings()
    {
        if (!_trackElement && _entity) {
            if (auto* element = _entity->findComponent<ElementComponent>()) {
                bindTrackElement(element);
                updateHandlePositionAndSize();
            }
        }
        if (!_handleElement && _handleEntity) {
            if (auto* element = _handleEntity->findComponent<ElementComponent>()) {
                bindHandleElement(element);
            }
        }
    }

    void ScrollbarComponent::bindTrackElement(ElementComponent* element)
    {
        _trackElement = element;
        _trackHandles.push_back(element->on("resize", [this]() { updateHandlePositionAndSize(); }));
        _trackHandles.push_back(element->on("beforeremove", [this]() { unbindTrackElement(); }));
    }

    void ScrollbarComponent::unbindTrackElement()
    {
        for (auto& handle : _trackHandles) {
            handle->off();
        }
        _trackHandles.clear();
        _trackElement = nullptr;
    }

    void ScrollbarComponent::bindHandleElement(ElementComponent* element)
    {
        _handleElement = element;
        _handleHandles.push_back(element->on("beforeremove", [this]() { unbindHandleElement(); }));
        for (const char* name : {"set:anchor", "set:margin", "set:pivot"}) {
            _handleHandles.push_back(element->on(name, [this]() { updateHandlePositionAndSize(); }));
        }
        rebuildDragHelper();
        updateHandlePositionAndSize();
    }

    void ScrollbarComponent::unbindHandleElement()
    {
        for (auto& handle : _handleHandles) {
            handle->off();
        }
        _handleHandles.clear();
        _dragHelper.reset();
        _handleElement = nullptr;
    }

    void ScrollbarComponent::rebuildDragHelper()
    {
        _dragHelper.reset();
        if (!_handleElement) {
            return;
        }
        _dragHelper = std::make_unique<ElementDragHelper>(
            _handleElement, _orientation == Orientation::Horizontal ? DragAxis::X : DragAxis::Y);
        // A helper built while the scrollbar is disabled must not start out draggable.
        _dragHelper->setEnabled(active());
        _dragHelper->on("drag:move", [this](const Vector3& position) {
            onHandleDrag(_orientation == Orientation::Horizontal ? position.getX() : position.getY());
        });
    }

    void ScrollbarComponent::onHandleDrag(const float position)
    {
        if (_handleEntity && active()) {
            setValue(position * sign() / usableTrackLength());
        }
    }

    void ScrollbarComponent::updateHandlePositionAndSize()
    {
        if (!_handleEntity) {
            return;
        }
        const float position = _value * sign() * usableTrackLength();
        Vector3 local = _handleEntity->localPosition();
        local = _orientation == Orientation::Horizontal ? Vector3(position, local.getY(), local.getZ())
                                                        : Vector3(local.getX(), position, local.getZ());
        _handleEntity->setLocalPosition(local);

        if (_handleElement) {
            if (_orientation == Orientation::Horizontal) {
                _handleElement->setWidth(handleLength());
            } else {
                _handleElement->setHeight(handleLength());
            }
        }
    }

    float ScrollbarComponent::trackLength() const
    {
        if (!_trackElement) {
            return 0.0f;
        }
        return _orientation == Orientation::Horizontal ? _trackElement->calculatedWidth()
                                                       : _trackElement->calculatedHeight();
    }

    float ScrollbarComponent::usableTrackLength() const
    {
        return std::max(trackLength() - handleLength(), 0.001f);
    }
}
