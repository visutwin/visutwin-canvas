// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2025.
//
#include "graphNode.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/math/matrix4.h"
#include "core/math/quaternion.h"
#include "core/math/vector4.h"

namespace visutwin::canvas
{
    GraphNode::~GraphNode()
    {
        // A manually deleted attached node must first release the parent's
        // unique_ptr without recursively deleting itself.
        if (_parent) {
            auto& siblings = _parent->_children;
            const auto it = std::find_if(siblings.begin(), siblings.end(), [this](const auto& child) {
                return child.get() == this;
            });
            if (it != siblings.end()) {
                it->release();
                siblings.erase(it);
            }
            _parent = nullptr;
        }
        for (auto& child : _children) {
            child->_parent = nullptr;
        }
        // _children owns and destroys the complete subtree.
    }

    void GraphNode::dirtifyLocal()
    {
        if (!_dirtyLocal) {
            _dirtyLocal = true;
            if (!_dirtyWorld) {
                dirtifyWorld();
            }
        }
    }

    void GraphNode::dirtifyWorld()
    {
        if (!_dirtyWorld) {
            unfreezeParentToRoot();
        }
        dirtifyWorldInternal();
    }

    void GraphNode::dirtifyWorldInternal()
    {
        if (!_dirtyWorld) {
            _frozen = false;
            _dirtyWorld = true;
            for (const auto& child : _children) {
                if (!child->_dirtyWorld) {
                    child->dirtifyWorldInternal();
                }
            }
        }
        _dirtyNormal = true;
        _worldScaleSign = 0;
        _aabbVer++;
    }

    void GraphNode::unfreezeParentToRoot()
    {
        auto* p = _parent;
        while (p) {
            p->_frozen = false;
            p = p->_parent;
        }
    }

    Quaternion GraphNode::rotation()
    {
        return Quaternion::fromMatrix4(worldTransform());
    }

    void GraphNode::setRotation(const Quaternion& rotation) {
        if (_parent == nullptr) {
            _localRotation = rotation;
        } else {
            _localRotation = _parent->rotation().invert() * rotation;
        }

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    void GraphNode::lookAt(const Vector3& target, const Vector3& up)
    {
        // The node's -Z looks at the target, so its +Z axis points back from it.
        const Vector3 back = target - position();
        if (back.lengthSquared() < 1e-12f) {
            return;  // degenerate: node sits on the target, keep the current rotation
        }
        const Vector3 zAxis = (-back).normalized();

        // Roll reference. When up is parallel to the view direction the cross product
        // collapses, so fall back to an axis that is guaranteed not to be parallel.
        Vector3 upAxis = up;
        Vector3 xAxis = upAxis.cross(zAxis);
        if (xAxis.lengthSquared() < 1e-12f) {
            upAxis = (std::abs(zAxis.getY()) > 0.999f) ? Vector3(0.0f, 0.0f, 1.0f)
                                                       : Vector3(0.0f, 1.0f, 0.0f);
            xAxis = upAxis.cross(zAxis);
        }
        xAxis = xAxis.normalized();
        const Vector3 yAxis = zAxis.cross(xAxis);

        // Columns are the basis vectors — the convention Quaternion::fromMatrix4 reads.
        const Matrix4 basis(
            Vector4(xAxis.getX(), xAxis.getY(), xAxis.getZ(), 0.0f),
            Vector4(yAxis.getX(), yAxis.getY(), yAxis.getZ(), 0.0f),
            Vector4(zAxis.getX(), zAxis.getY(), zAxis.getZ(), 0.0f),
            Vector4(0.0f, 0.0f, 0.0f, 1.0f)
        );
        setRotation(Quaternion::fromMatrix4(basis));
    }

    void GraphNode::lookAt(const float tx, const float ty, const float tz,
        const float ux, const float uy, const float uz)
    {
        lookAt(Vector3(tx, ty, tz), Vector3(ux, uy, uz));
    }

    void GraphNode::setLocalRotation(const Quaternion& rotation)
    {
        _localRotation = rotation;

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    const Matrix4& GraphNode::worldTransform() {
        if (!_dirtyLocal && !_dirtyWorld) {
            return _worldTransform;
        }

        if (_parent) {
            _parent->worldTransform();
        }

        sync();
        return _worldTransform;
    }

    float GraphNode::worldScaleSign()
    {
        if (_worldScaleSign == 0) {
            const auto& wt = worldTransform();
            const float m00 = wt.getElement(0, 0);
            const float m01 = wt.getElement(0, 1);
            const float m02 = wt.getElement(0, 2);
            const float m10 = wt.getElement(1, 0);
            const float m11 = wt.getElement(1, 1);
            const float m12 = wt.getElement(1, 2);
            const float m20 = wt.getElement(2, 0);
            const float m21 = wt.getElement(2, 1);
            const float m22 = wt.getElement(2, 2);
            const float det3 = m00 * (m11 * m22 - m12 * m21) -
                m01 * (m10 * m22 - m12 * m20) +
                m02 * (m10 * m21 - m11 * m20);
            _worldScaleSign = det3 < 0.0f ? -1 : 1;
        }
        return static_cast<float>(_worldScaleSign);
    }

    void GraphNode::sync()
    {
        if (_dirtyLocal) {
            _localTransform = Matrix4::trs(_localPosition, _localRotation, _localScale);
            _dirtyLocal = false;
        }

        if (_dirtyWorld) {
            if (_parent == nullptr) {
                _worldTransform = _localTransform;
            } else {
                if (_scaleCompensation) {
                    // Scale compensation logic (complex)
                    Vector3 parentWorldScale;

                    Vector3 scale = _localScale;

                    if (auto* parentToUseScaleFrom = _parent) {
                        while (parentToUseScaleFrom && parentToUseScaleFrom->_scaleCompensation) {
                            parentToUseScaleFrom = parentToUseScaleFrom->_parent;
                        }
                        if (parentToUseScaleFrom) {
                            parentToUseScaleFrom = parentToUseScaleFrom->_parent;
                            if (parentToUseScaleFrom) {
                                parentWorldScale = parentToUseScaleFrom->worldTransform().getScale();
                                scale = parentWorldScale * _localScale;
                            }
                        }
                    }

                    const Quaternion scaleCompensateRot2 = Quaternion::fromMatrix4(_parent->worldTransform());
                    const Quaternion scaleCompensateRot = scaleCompensateRot2 * _localRotation;

                    Vector3 scaleCompensatePos;
                    Matrix4 tmatrix = _parent->worldTransform();
                    if (_parent->_scaleCompensation) {
                        Vector3 scaleCompensateScaleForParent = parentWorldScale * _parent->localScale();
                        scaleCompensatePos = _parent->worldTransform().getTranslation();
                        tmatrix = Matrix4::trs(scaleCompensatePos, scaleCompensateRot2, scaleCompensateScaleForParent);
                    }
                    scaleCompensatePos = tmatrix.transformPoint(_localPosition);

                    _worldTransform = Matrix4::trs(scaleCompensatePos, scaleCompensateRot, scale);
                } else {
                    _worldTransform = _parent->worldTransform().mulAffine(_localTransform);
                }
            }

            _dirtyWorld = false;
        }
    }

    void GraphNode::setLocalEulerAngles(float x, float y, float z)
    {
        _localRotation = Quaternion::fromEulerAngles(x, y, z);

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    void GraphNode::translateLocal(float x, float y, float z)
    {
        // 
        // Transform the translation vector by the local rotation, then add to local position.
        const Vector3 offset = _localRotation * Vector3(x, y, z);
        _localPosition = _localPosition + offset;

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    void GraphNode::rotateLocal(float x, float y, float z)
    {
        const Quaternion rotation = Quaternion::fromEulerAngles(x, y, z);
        _localRotation = _localRotation * rotation;

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    void GraphNode::addChild(std::unique_ptr<GraphNode> node)
    {
        validateInsertChild(node.get());
        if (node->_parent) {
            throw std::invalid_argument("Cannot transfer unique ownership of an attached GraphNode");
        }

        auto* raw = node.get();
        _children.push_back(std::move(node));
        onInsertChild(raw);
    }

    void GraphNode::addChild(GraphNode* node)
    {
        validateInsertChild(node);

        std::unique_ptr<GraphNode> ownership;
        if (node->_parent) {
            ownership = node->_parent->removeChild(node);
        } else {
            ownership.reset(node);
        }

        auto* raw = ownership.get();
        _children.push_back(std::move(ownership));
        onInsertChild(raw);
    }

    bool GraphNode::isDescendantOf(const GraphNode* node) const
    {
        auto* parent = _parent;
        while (parent) {
            if (parent == node) {
                return true;
            }
            parent = parent->_parent;
        }
        return false;
    }

    std::unique_ptr<GraphNode> GraphNode::remove()
    {
        if (_parent) {
            return _parent->removeChild(this);
        }
        return nullptr;
    }

    std::unique_ptr<GraphNode> GraphNode::removeChild(GraphNode* child)
    {
        auto it = std::find_if(_children.begin(), _children.end(), [child](const auto& candidate) {
            return candidate.get() == child;
        });
        if (it == _children.end()) {
            return nullptr;
        }

        auto ownership = std::move(*it);
        _children.erase(it);
        child->_parent = nullptr;
        child->updateGraphDepth();
        child->dirtifyWorld();
        if (child->_enabledInHierarchy) {
            child->notifyHierarchyStateChanged(child, false);
        }
        child->fireOnHierarchy("remove", "removehierarchy", this);
        fire("childremove", child);
        return ownership;
    }

    void GraphNode::fireOnHierarchy(const std::string& name, const std::string& nameHierarchy, GraphNode* parent)
    {
        fire(name, parent);
        for (const auto& child : _children) {
            child->fireOnHierarchy(nameHierarchy, nameHierarchy, parent);
        }
    }

    void GraphNode::validateInsertChild(const GraphNode* node) const
    {
        if (!node) {
            throw std::invalid_argument("Cannot add a null GraphNode child");
        }
        if (node == this) {
            throw std::invalid_argument("A GraphNode cannot be a child of itself");
        }
        if (isDescendantOf(node)) {
            throw std::invalid_argument("A GraphNode cannot add one of its ancestors as a child");
        }
    }

    void GraphNode::onInsertChild(GraphNode* node)
    {
        node->_parent = this;

        // A child is enabled-in-hierarchy only if BOTH the parent hierarchy
        // is enabled AND the child's own _enabled flag is true.
        // Mirrors upstream: node._enabledInHierarchy = parent.enabled && node._enabled
        bool enabledInHierarchy = enabled() && node->_enabled;
        if (node->_enabledInHierarchy != enabledInHierarchy) {
            node->_enabledInHierarchy = enabledInHierarchy;
            node->notifyHierarchyStateChanged(node, enabledInHierarchy);
        }

        node->updateGraphDepth();
        node->dirtifyWorld();

        if (_frozen) {
            node->unfreezeParentToRoot();
        }

        node->fireOnHierarchy("insert", "inserthierarchy", this);
        fire("childinsert", node);
    }

    void GraphNode::notifyHierarchyStateChanged(GraphNode* node, bool enabled)
    {
        node->onHierarchyStateChanged(enabled);

        for (const auto& child : node->_children) {
            if (child->_enabled) {
                notifyHierarchyStateChanged(child.get(), enabled);
            }
        }
    }

    void GraphNode::setEnabled(bool value)
    {
        // Mirrors upstream GraphNode enabled setter.
        if (_enabled != value) {
            _enabled = value;

            // A parentless node is a hierarchy root, so enabling it must also
            // restore its hierarchy state. Attached nodes can only become
            // enabled-in-hierarchy when their parent is enabled.
            if (!value || !_parent || _parent->enabled()) {
                notifyHierarchyStateChanged(this, value);
            }
        }
    }

    void GraphNode::onHierarchyStateChanged(bool enabled)
    {
        _enabledInHierarchy = enabled;
        if (enabled && !_frozen) {
            unfreezeParentToRoot();
        }
    }

    void GraphNode::updateGraphDepth()
    {
        _graphDepth = _parent ? _parent->_graphDepth + 1 : 0;

        for (const auto& child : _children) {
            child->updateGraphDepth();
        }
    }

    void GraphNode::setLocalPosition(float x, float y, float z)
    {
        _localPosition = Vector3(x, y, z);

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    void GraphNode::setLocalPosition(const Vector3& position)
    {
        setLocalPosition(position.getX(), position.getY(), position.getZ());
    }

    void GraphNode::setLocalScale(float x, float y, float z)
    {
        setLocalScale(Vector3(x, y, z));
    }

    void GraphNode::setLocalScale(const Vector3& scale)
    {
        _localScale = scale;

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    const Vector3 GraphNode::position()
    {
        _position = worldTransform().getTranslation();
        return _position;
    }

    void GraphNode::setPosition(float x, float y, float z)
    {
        setPosition(Vector3(x, y, z));
    }

    void GraphNode::setPosition(const Vector3& position)
    {
        if (_parent == nullptr) {
            _localPosition = position;
        } else {
            const Matrix4 invParentWtm = _parent->worldTransform().inverse();
            _localPosition = invParentWtm.transformPoint(position);
        }

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    GraphNode* GraphNode::findByName(const std::string& name)
    {
        if (_name == name) {
            return this;
        }

        for (const auto& child : _children) {
            if (GraphNode* found = child->findByName(name)) {
                return found;
            }
        }

        return nullptr;
    }

    std::vector<GraphNode*> GraphNode::find(const std::function<bool(GraphNode*)>& predicate)
    {
        std::vector<GraphNode*> results;

        if (predicate(this)) {
            results.push_back(this);
        }

        for (const auto& child : _children) {
            auto childResults = child->find(predicate);
            results.insert(results.end(), childResults.begin(), childResults.end());
        }

        return results;
    }
}
