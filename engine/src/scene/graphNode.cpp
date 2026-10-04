// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 07.10.2025
//
#include "graphNode.h"
#include "graphNodeTransformHook.h"

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
            if (const size_t slot = _parent->childSlot(this); slot != SIZE_MAX) {
                _parent->_children[slot].release();
                if (slot + 1 == _parent->_children.size()) {
                    _parent->_children.pop_back();
                } else {
                    ++_parent->_childHoles;
                }
            }
            _parent = nullptr;
        }
        for (auto& child : _children) {
            if (child) {
                child->_parent = nullptr;
            }
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
                if (child && !child->_dirtyWorld) {
                    child->dirtifyWorldInternal();
                }
            }
        }
        _dirtyNormal = true;
        _dirtyWorldRotation = true;
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
        // worldTransform() syncs first, so a clean transform here means the cached
        // rotation is still valid; the decomposition only reruns after a dirtify.
        const Matrix4& world = worldTransform();
        if (_dirtyWorldRotation) {
            _worldRotation = Quaternion::fromMatrix4(world);
            _dirtyWorldRotation = false;
        }
        return _worldRotation;
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
            Vector4(xAxis, 0.0f),
            Vector4(yAxis, 0.0f),
            Vector4(zAxis, 0.0f),
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
            const float det3 = worldTransform().determinant3x3();
            _worldScaleSign = det3 < 0.0f ? -1 : 1;
        }
        return static_cast<float>(_worldScaleSign);
    }

    void GraphNode::setTransformHook(GraphNodeTransformHook* hook)
    {
        _transformHook = hook;
        dirtifyLocal();
    }

    void GraphNode::sync()
    {
        if (_transformHook) {
            _transformHook->syncTransform(*this);
            return;
        }
        defaultSync();
    }

    void GraphNode::defaultSync()
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

    void GraphNode::rotate(const float x, const float y, const float z)
    {
        const Quaternion rotation = Quaternion::fromEulerAngles(x, y, z);

        if (_parent == nullptr) {
            // No parent, so local space IS world space — pre-multiply directly.
            _localRotation = rotation * _localRotation;
        } else {
            // Bring the world-space rotation into the parent's frame before applying
            // it to the node's world orientation, then store the result as local.
            // Expanding this node's world rotation as parent * local keeps the parent
            // as the only node queried: asking for our OWN world rotation would sync
            // a world transform that the dirtify below immediately invalidates.
            const Quaternion parentRotation = _parent->rotation();
            const Quaternion parentInverse = parentRotation.invert();
            _localRotation = (parentInverse * rotation) * (parentRotation * _localRotation);
        }

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
    }

    void GraphNode::rotate(const Vector3& eulerAngles)
    {
        rotate(eulerAngles.getX(), eulerAngles.getY(), eulerAngles.getZ());
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
        raw->_slotInParent = _children.size();
        _children.push_back(std::move(node));
        onInsertChild(raw);
    }

    void GraphNode::addChild(GraphNode* node)
    {
        validateInsertChild(node);

        std::unique_ptr<GraphNode> ownership;
        if (node->_parent) {
            // A move: the node keeps its enabled-in-hierarchy state across the detach, and
            // onInsertChild notifies the subtree only if the new parent changes it.
            ownership = node->_parent->detachChild(node, false);
        } else {
            ownership.reset(node);
        }

        auto* raw = ownership.get();
        raw->_slotInParent = _children.size();
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

    size_t GraphNode::childSlot(const GraphNode* child) const
    {
        if (!child || child->_parent != this) {
            return SIZE_MAX;
        }
        // The child knows its slot; the scan is only a guard against a slot gone stale.
        const size_t slot = child->_slotInParent;
        if (slot < _children.size() && _children[slot].get() == child) {
            return slot;
        }
        for (size_t i = 0; i < _children.size(); ++i) {
            if (_children[i].get() == child) {
                return i;
            }
        }
        return SIZE_MAX;
    }

    void GraphNode::compactChildren() const
    {
        size_t write = 0;
        for (size_t read = 0; read < _children.size(); ++read) {
            if (_children[read]) {
                if (write != read) {
                    _children[write] = std::move(_children[read]);
                }
                _children[write]->_slotInParent = write;
                ++write;
            }
        }
        _children.resize(write);
        _childHoles = 0;
    }

    std::unique_ptr<GraphNode> GraphNode::removeChild(GraphNode* child)
    {
        // DEVIATION: a detached subtree is disabled in hierarchy here, where upstream leaves
        // it enabled. Rendering, lighting and the other per-frame gathers walk global
        // component lists filtered by active(), not the scene graph, so a detached entity
        // that stayed active would keep rendering, lighting and simulating while in no
        // scene. Moving a node between parents with addChild does not pass through this
        // state (detachChild with disableDetached false).
        return detachChild(child, true);
    }

    std::unique_ptr<GraphNode> GraphNode::detachChild(GraphNode* child, const bool disableDetached)
    {
        const size_t slot = childSlot(child);
        if (slot == SIZE_MAX) {
            return nullptr;
        }

        // A hole, not an erase: see children(). The last child is simply popped, which
        // keeps the list dense for the common remove-from-the-back loop.
        auto ownership = std::move(_children[slot]);
        if (slot + 1 == _children.size()) {
            _children.pop_back();
        } else {
            ++_childHoles;
        }
        child->_parent = nullptr;
        child->updateGraphDepth();
        child->dirtifyWorld();
        if (disableDetached && child->_enabledInHierarchy) {
            child->notifyHierarchyStateChanged(child, false);
        }
        child->fireOnHierarchy("remove", "removehierarchy", this);
        fire("childremove", child);
        return ownership;
    }

    void GraphNode::fireOnHierarchy(const std::string& name, const std::string& nameHierarchy, GraphNode* parent)
    {
        fire(name, parent);
        // By index, skipping holes: a handler may remove a child (see children()).
        for (size_t i = 0; i < _children.size(); ++i) {
            if (GraphNode* child = _children[i].get()) {
                child->fireOnHierarchy(nameHierarchy, nameHierarchy, parent);
            }
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
        node->onInsertedIntoParent();

        // A child is enabled-in-hierarchy only if BOTH the parent hierarchy
        // is enabled AND the child's own _enabled flag is true.
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

        // By index, skipping holes: an enable or disable hook may remove a child.
        for (size_t i = 0; i < node->_children.size(); ++i) {
            GraphNode* child = node->_children[i].get();
            if (child && child->_enabled) {
                notifyHierarchyStateChanged(child, enabled);
            }
        }
    }

    void GraphNode::setEnabled(bool value)
    {
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
            if (child) {
                child->updateGraphDepth();
            }
        }
    }

    void GraphNode::setLocalPosition(float x, float y, float z)
    {
        setLocalPosition(Vector3(x, y, z));
    }

    void GraphNode::setLocalPosition(const Vector3& position)
    {
        if (_transformHook) {
            _transformHook->setNodeLocalPosition(*this, position);
            return;
        }
        defaultSetLocalPosition(position);
    }

    void GraphNode::defaultSetLocalPosition(const Vector3& position)
    {
        _localPosition = position;

        if (!_dirtyLocal) {
            dirtifyLocal();
        }
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
        if (_transformHook) {
            _transformHook->setNodePosition(*this, position);
            return;
        }
        defaultSetPosition(position);
    }

    void GraphNode::defaultSetPosition(const Vector3& position)
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
            if (GraphNode* found = child ? child->findByName(name) : nullptr) {
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
            if (!child) {
                continue;
            }
            auto childResults = child->find(predicate);
            results.insert(results.end(), childResults.begin(), childResults.end());
        }

        return results;
    }
}
