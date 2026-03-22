// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 05.09.2025.
//
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/eventHandler.h"
#include "core/tags.h"
#include "core/math/quaternion.h"

namespace visutwin::canvas
{
    /**
     * The GraphNode class represents a node within a hierarchical scene graph. Each GraphNode can
     * reference an array of children. This creates a tree-like structure that is fundamental
     * for organizing and managing the spatial relationships between objects in a 3D scene. This class
     * provides a comprehensive API for manipulating the position, rotation, and scale of nodes both
     * locally (relative to the parent) and in world space (relative to the Scene origin).
     *
     * During the application's main update loop, the engine automatically synchronizes the entire
     * GraphNode hierarchy each frame. This process ensures that the world transformation matrices
     * for all nodes are up to date. A node's world transformation matrix is calculated by combining
     * its local transformation matrix (derived from its local position, rotation, and scale) with
     * the world transformation matrix of its parent node.
     */
    class GraphNode : public EventHandler
    {
    public:
        explicit GraphNode(const std::string& name = "Untitled")
            : _name(name), _tags(this), _localScale(1, 1, 1),
              _localTransform(Matrix4::identity()), _worldTransform(Matrix4::identity()) {}
        virtual ~GraphNode();

        const std::string& name() const { return _name; }
        void setName(const std::string& name) { _name = name; }

        Quaternion rotation();
        void setRotation(const Quaternion& rotation);
        void setLocalRotation(const Quaternion& rotation);

        const Matrix4& worldTransform();

        const Vector3& localPosition() const { return _localPosition; }
        const Quaternion& localRotation() const { return _localRotation; }
        const Vector3& localScale() const { return _localScale; }
        void setLocalScale(float x, float y, float z);
        void setLocalScale(const Vector3& scale);

        void setLocalEulerAngles(float x, float y, float z);

        /**
         * Rotates the graph node in local space by the specified Euler angles.
         * Eulers are specified in degrees in XYZ order.
         */
        void rotateLocal(float x, float y, float z);

        /**
         * Translates the graph node in local space by the specified amounts.
         * .
         */
        void translateLocal(float x, float y, float z);

        void setLocalPosition(float x, float y, float z);
        void setLocalPosition(const Vector3& position);

        /**
         * Get the world space position for the specified GraphNode. The position is returned as a
         * {@link Vector3}. The value returned by this function should be considered read-only.
         * To set the world space position of the graph node, use {@link setPosition}.
         */
        const Vector3 position();

        void setPosition(float x, float y, float z);
        void setPosition(const Vector3& position);

        void addChild(GraphNode* node);

        const std::vector<GraphNode*>& children() const { return _children; }

        GraphNode* findByName(const std::string& name);

        /// Find all descendants (and self) matching a predicate.
        std::vector<GraphNode*> find(const std::function<bool(GraphNode*)>& predicate);

        bool isDescendantOf(const GraphNode* node) const;

        GraphNode* parent() const { return _parent; }

        void removeChild(GraphNode* child);

        /**
        * Remove graph node from current parent
        */
        void remove();

        bool enabled() const { return _enabled && _enabledInHierarchy; }

        /**
         * Returns the local enabled flag (without hierarchy consideration).
         *(used by _cloneInternal).
         */
        bool enabledLocal() const { return _enabled; }

        /**
         * Set the enabled flag and propagate hierarchy state to children.
         * Mirrors upstream `set enabled(value)`.
         */
        void setEnabled(bool value);

        /**
         * Directly set the _enabledInHierarchy flag. Used for root nodes that
         * have no parent. Mirrors upstream `root._enabledInHierarchy = true`.
         */
        void setEnabledInHierarchy(bool value) { _enabledInHierarchy = value; }

        int aabbVer() const { return _aabbVer;}
        float worldScaleSign();

    protected:
        virtual void onHierarchyStateChanged(bool enabled);

        void notifyHierarchyStateChanged(GraphNode* node, bool enabled);

    private:
        void dirtifyLocal();

        void dirtifyWorld();

        void unfreezeParentToRoot();

        void dirtifyWorldInternal();

        void sync();

        void fireOnHierarchy(const std::string& name, const std::string& nameHierarchy, GraphNode* parent);

        void prepareInsertChild(GraphNode* node);

        void onInsertChild(GraphNode* node);

        void updateGraphDepth();

        std::string _name;

        Tags _tags;

        // Local space properties of transform
        Vector3 _localPosition;
        Quaternion _localRotation;
        Vector3 _localScale;

        Vector3 _position;

        // Transform matrices
        Matrix4 _localTransform;
        Matrix4 _worldTransform;

        // Version tracking
        int _aabbVer = 0;
        int _worldScaleSign = 0;

        GraphNode* _parent = nullptr;
        std::vector<GraphNode*> _children;
        int _graphDepth = 0;

        bool _dirtyLocal = false;
        bool _dirtyWorld = false;
        bool _dirtyNormal = true;
        bool _frozen = false;

        bool _scaleCompensation = false;

        bool _enabled = true;
        bool _enabledInHierarchy = false;
    };
}
