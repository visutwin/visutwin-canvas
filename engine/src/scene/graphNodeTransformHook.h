// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 29.09.2026
//
// A node's transform, computed by something other than the node.
//
// A UI element's world transform is not parent x local, it goes through its anchors, its
// parent element's model transform and its screen's projection, and setting its position
// also re-derives its margins. C++ cannot swap a member function on one object, so a
// GraphNode carries an optional hook that replaces its sync, setPosition and
// setLocalPosition, installed and removed by the element. A node without one pays a single
// pointer test.
//
#pragma once

#include "graphNode.h"

namespace visutwin::canvas
{
    class GraphNodeTransformHook
    {
    public:
        virtual ~GraphNodeTransformHook() = default;

        /// Brings the node's local and world transforms up to date. Called in place of
        /// GraphNode's own sync, after the parent's world transform has been synced.
        virtual void syncTransform(GraphNode& node) = 0;

        /// In place of GraphNode::setPosition / setLocalPosition.
        virtual void setNodePosition(GraphNode& node, const Vector3& position) = 0;
        virtual void setNodeLocalPosition(GraphNode& node, const Vector3& position) = 0;

    protected:
        // The node's own sync, position setters and fields.
        static void defaultSync(GraphNode& node) { node.defaultSync(); }
        static void defaultSetPosition(GraphNode& node, const Vector3& p) { node.defaultSetPosition(p); }
        static void defaultSetLocalPosition(GraphNode& node, const Vector3& p) { node.defaultSetLocalPosition(p); }

        static Matrix4& localTransform(GraphNode& node) { return node._localTransform; }
        static Matrix4& worldTransformStorage(GraphNode& node) { return node._worldTransform; }
        static Vector3& localPositionStorage(GraphNode& node) { return node._localPosition; }
        static bool& dirtyLocal(GraphNode& node) { return node._dirtyLocal; }
        static bool& dirtyWorld(GraphNode& node) { return node._dirtyWorld; }
        static void dirtifyLocal(GraphNode& node) { node.dirtifyLocal(); }
        static void dirtifyWorld(GraphNode& node) { node.dirtifyWorld(); }
    };
}
