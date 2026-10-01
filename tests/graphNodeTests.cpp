// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <stdexcept>
#include <string_view>

#include "scene/graphNode.h"
#include "framework/components/component.h"
#include "framework/entity.h"

using visutwin::canvas::Component;
using visutwin::canvas::componentTypeID;
using visutwin::canvas::Entity;
using visutwin::canvas::GraphNode;
using visutwin::canvas::Vector3;

namespace
{
    class TestComponent final : public Component
    {
    public:
        explicit TestComponent(Entity* entity) : Component(nullptr, entity) {}
        void initializeComponentData() override {}
    };

    class OtherComponent final : public Component
    {
    public:
        explicit OtherComponent(Entity* entity) : Component(nullptr, entity) {}
        void initializeComponentData() override {}
    };

    bool near(const float lhs, const float rhs)
    {
        return std::abs(lhs - rhs) <= 1e-5f;
    }

    bool expect(const bool condition, const std::string_view message)
    {
        if (!condition) {
            std::cerr << "FAILED: " << message << '\n';
        }
        return condition;
    }

    // The node's world-space +X axis, which is what a rotation is observable through.
    Vector3 worldAxisX(GraphNode* node)
    {
        const auto& m = node->worldTransform();
        return Vector3(m.getElement(0, 0), m.getElement(0, 1), m.getElement(0, 2)).normalized();
    }

    // Same observation, but through rotation() — the cached quaternion — so that a
    // stale cache is visible. worldAxisX() reads the matrix and would not notice.
    bool expectRotationAxis(GraphNode* node, const Vector3& expected,
        const std::string_view message)
    {
        const auto actual = (node->rotation() * Vector3(1.0f, 0.0f, 0.0f)).normalized();
        return expect(
            near(actual.getX(), expected.getX()) &&
            near(actual.getY(), expected.getY()) &&
            near(actual.getZ(), expected.getZ()),
            message);
    }

    bool expectAxis(GraphNode* node, const Vector3& expected, const std::string_view message)
    {
        const auto actual = worldAxisX(node);
        return expect(
            near(actual.getX(), expected.getX()) &&
            near(actual.getY(), expected.getY()) &&
            near(actual.getZ(), expected.getZ()),
            message);
    }

    bool expectPosition(GraphNode* node, const Vector3& expected, const std::string_view message)
    {
        const auto actual = node->position();
        return expect(
            near(actual.getX(), expected.getX()) &&
            near(actual.getY(), expected.getY()) &&
            near(actual.getZ(), expected.getZ()),
            message);
    }

    template <typename Operation>
    bool expectInvalidInsertion(Operation&& operation, const std::string_view message)
    {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        } catch (...) {
            std::cerr << "FAILED: " << message << " threw the wrong exception\n";
            return false;
        }

        std::cerr << "FAILED: " << message << " was accepted\n";
        return false;
    }
}

int main()
{
    auto firstRoot = std::make_unique<GraphNode>("first-root");
    firstRoot->setEnabledInHierarchy(true);
    firstRoot->setLocalPosition(10.0f, 0.0f, 0.0f);

    auto branch = std::make_unique<GraphNode>("branch");
    auto* branchObserver = branch.get();
    branch->setLocalPosition(2.0f, 0.0f, 0.0f);

    auto leaf = std::make_unique<GraphNode>("leaf");
    auto* leafObserver = leaf.get();
    leaf->setLocalPosition(3.0f, 0.0f, 0.0f);
    branch->addChild(std::move(leaf));
    firstRoot->addChild(std::move(branch));

    bool passed = true;
    passed &= expectPosition(branchObserver, Vector3(12.0f, 0.0f, 0.0f),
        "attached branch world transform");
    passed &= expectPosition(leafObserver, Vector3(15.0f, 0.0f, 0.0f),
        "attached leaf world transform");
    passed &= expect(branchObserver->graphDepth() == 1, "attached branch depth");
    passed &= expect(leafObserver->graphDepth() == 2, "attached leaf depth");
    passed &= expect(branchObserver->enabled() && leafObserver->enabled(),
        "attached subtree enabled state");

    const int branchAabbVersion = branchObserver->aabbVer();
    const int leafAabbVersion = leafObserver->aabbVer();
    auto detached = firstRoot->removeChild(branchObserver);

    passed &= expect(detached.get() == branchObserver, "detach transfers ownership");
    passed &= expect(branchObserver->parent() == nullptr, "detached branch parent");
    passed &= expect(branchObserver->graphDepth() == 0, "detached branch depth");
    passed &= expect(leafObserver->graphDepth() == 1, "detached leaf depth");
    passed &= expect(!branchObserver->enabled() && !leafObserver->enabled(),
        "detached subtree hierarchy state");
    passed &= expect(branchObserver->aabbVer() > branchAabbVersion,
        "detached branch AABB cache invalidated");
    passed &= expect(leafObserver->aabbVer() > leafAabbVersion,
        "detached leaf AABB cache invalidated");
    passed &= expectPosition(branchObserver, Vector3(2.0f, 0.0f, 0.0f),
        "detached branch world transform recomputed");
    passed &= expectPosition(leafObserver, Vector3(5.0f, 0.0f, 0.0f),
        "detached leaf world transform recomputed");

    auto secondRoot = std::make_unique<GraphNode>("second-root");
    secondRoot->setEnabledInHierarchy(true);
    secondRoot->setLocalPosition(-4.0f, 0.0f, 0.0f);
    secondRoot->addChild(std::move(detached));

    passed &= expect(branchObserver->parent() == secondRoot.get(), "reparented branch parent");
    passed &= expect(branchObserver->graphDepth() == 1, "reparented branch depth");
    passed &= expect(leafObserver->graphDepth() == 2, "reparented leaf depth");
    passed &= expect(branchObserver->enabled() && leafObserver->enabled(),
        "reparented subtree enabled state");
    passed &= expectPosition(branchObserver, Vector3(-2.0f, 0.0f, 0.0f),
        "reparented branch world transform recomputed");
    passed &= expectPosition(leafObserver, Vector3(1.0f, 0.0f, 0.0f),
        "reparented leaf world transform recomputed");

    passed &= expectInvalidInsertion(
        [&] { secondRoot->addChild(secondRoot.get()); },
        "self insertion");
    passed &= expectInvalidInsertion(
        [&] { leafObserver->addChild(secondRoot.get()); },
        "ancestor insertion");
    passed &= expectInvalidInsertion(
        [&] { secondRoot->addChild(static_cast<GraphNode*>(nullptr)); },
        "null raw child insertion");
    passed &= expectInvalidInsertion(
        [&] { secondRoot->addChild(std::unique_ptr<GraphNode>{}); },
        "null owned child insertion");

    passed &= expect(secondRoot->parent() == nullptr,
        "invalid insertion preserves root parent");
    passed &= expect(secondRoot->children().size() == 1,
        "invalid insertion preserves root children");
    passed &= expect(branchObserver->parent() == secondRoot.get(),
        "invalid insertion preserves branch parent");
    passed &= expect(branchObserver->children().size() == 1,
        "invalid insertion preserves branch children");
    passed &= expect(leafObserver->parent() == branchObserver,
        "invalid insertion preserves leaf parent");
    passed &= expect(branchObserver->graphDepth() == 1 && leafObserver->graphDepth() == 2,
        "invalid insertion preserves graph depth");
    passed &= expectPosition(leafObserver, Vector3(1.0f, 0.0f, 0.0f),
        "invalid insertion preserves world transform");

    secondRoot->setEnabled(false);
    passed &= expect(!secondRoot->enabled(), "disabled root hierarchy state");
    passed &= expect(!branchObserver->enabled() && !leafObserver->enabled(),
        "disabled root propagates to descendants");

    secondRoot->setEnabled(true);
    passed &= expect(secondRoot->enabled(), "root can be re-enabled");
    passed &= expect(branchObserver->enabled() && leafObserver->enabled(),
        "re-enabled root propagates to descendants");

    auto entityRoot = std::make_unique<Entity>();
    auto* rootComponent = static_cast<TestComponent*>(entityRoot->addComponentInstance(
        std::make_unique<TestComponent>(entityRoot.get()), componentTypeID<TestComponent>()));

    auto intermediateNode = std::make_unique<GraphNode>("non-entity-intermediate");
    auto childEntity = std::make_unique<Entity>();
    auto* childEntityObserver = childEntity.get();
    auto* childComponent = static_cast<TestComponent*>(childEntity->addComponentInstance(
        std::make_unique<TestComponent>(childEntity.get()), componentTypeID<TestComponent>()));
    childEntity->addComponentInstance(
        std::make_unique<OtherComponent>(childEntity.get()), componentTypeID<OtherComponent>());
    intermediateNode->addChild(std::move(childEntity));

    auto emptyEntity = std::make_unique<Entity>();
    auto descendantEntity = std::make_unique<Entity>();
    auto* descendantComponent = static_cast<TestComponent*>(descendantEntity->addComponentInstance(
        std::make_unique<TestComponent>(descendantEntity.get()), componentTypeID<TestComponent>()));
    emptyEntity->addChild(std::move(descendantEntity));

    entityRoot->addChild(std::move(intermediateNode));
    entityRoot->addChild(std::move(emptyEntity));

    const auto foundComponents = entityRoot->findComponents<TestComponent>();
    passed &= expect(foundComponents.size() == 3,
        "findComponents returns matches from self and all descendants");
    passed &= expect(std::ranges::find(foundComponents, rootComponent) != foundComponents.end(),
        "findComponents includes receiver component");
    passed &= expect(std::ranges::find(foundComponents, childComponent) != foundComponents.end(),
        "findComponents traverses non-entity graph nodes");
    passed &= expect(std::ranges::find(foundComponents, descendantComponent) != foundComponents.end(),
        "findComponents traverses entities without a local match");
    passed &= expect(childEntityObserver->findComponents<OtherComponent>().size() == 1,
        "findComponents filters by component type");

    // ---- world-space rotate vs local-space rotateLocal -------------------------
    // A single rotation from identity is the same either way...
    {
        auto worldNode = std::make_unique<GraphNode>("rotate-world");
        auto localNode = std::make_unique<GraphNode>("rotate-local");
        worldNode->rotate(0.0f, 90.0f, 0.0f);
        localNode->rotateLocal(0.0f, 90.0f, 0.0f);
        passed &= expectAxis(worldNode.get(), Vector3(0.0f, 0.0f, -1.0f),
            "rotate about world Y turns +X to -Z");
        passed &= expectAxis(localNode.get(), Vector3(0.0f, 0.0f, -1.0f),
            "rotateLocal matches rotate from identity");
    }

    // ...but they diverge once the node is already tilted. Roll 90 about Z, which puts
    // the node's +X along world +Y and its own Y along world -X. Then yaw 90:
    //   world-space: turns about WORLD Y, which +X is now parallel to -> +X unmoved
    //   local-space: turns about the node's OWN Y (world -X)          -> +X ends at -Z
    {
        auto worldNode = std::make_unique<GraphNode>("tilted-world");
        worldNode->rotate(0.0f, 0.0f, 90.0f);
        worldNode->rotate(0.0f, 90.0f, 0.0f);
        passed &= expectAxis(worldNode.get(), Vector3(0.0f, 1.0f, 0.0f),
            "second rotate is about the world axis, not the node's");

        auto localNode = std::make_unique<GraphNode>("tilted-local");
        localNode->rotateLocal(0.0f, 0.0f, 90.0f);
        localNode->rotateLocal(0.0f, 90.0f, 0.0f);
        passed &= expectAxis(localNode.get(), Vector3(0.0f, 0.0f, -1.0f),
            "rotateLocal turns about the node's own axis");
    }

    // Under a rotated parent, rotate() must still act in WORLD space: the parent's
    // 90-degree yaw is undone before the rotation is stored as local.
    {
        auto parent = std::make_unique<GraphNode>("rotated-parent");
        parent->setLocalEulerAngles(0.0f, 90.0f, 0.0f);
        auto child = std::make_unique<GraphNode>("child");
        auto* childObserver = child.get();
        parent->addChild(std::move(child));

        childObserver->rotate(0.0f, -90.0f, 0.0f);
        passed &= expectAxis(childObserver, Vector3(1.0f, 0.0f, 0.0f),
            "rotate under a rotated parent cancels the parent yaw in world space");
    }

    // The world rotation is CACHED alongside the world transform, so these guard the
    // invalidation rather than the maths: a stale cache returns a plausible-looking
    // orientation and would otherwise fail silently.
    {
        auto node = std::make_unique<GraphNode>("cache-invalidate");
        node->rotation();                                   // prime the cache
        node->setLocalEulerAngles(0.0f, 90.0f, 0.0f);       // must invalidate it
        passed &= expectRotationAxis(node.get(), Vector3(0.0f, 0.0f, -1.0f),
            "rotation() reflects a local rotation set after it was first read");

        // Reading twice must be stable — the second read comes from the cache.
        const visutwin::canvas::Quaternion first = node->rotation();
        const visutwin::canvas::Quaternion second = node->rotation();
        const bool stable = std::fabs(first.getX() - second.getX()) < 1e-6f &&
                            std::fabs(first.getY() - second.getY()) < 1e-6f &&
                            std::fabs(first.getZ() - second.getZ()) < 1e-6f &&
                            std::fabs(first.getW() - second.getW()) < 1e-6f;
        if (!stable) {
            std::cerr << "FAIL: repeated rotation() reads disagree\n";
        }
        passed &= stable;
    }

    // A parent's rotation must invalidate the child's cached world rotation, which is
    // the case a per-node dirty flag gets wrong if it does not propagate downwards.
    {
        auto parent = std::make_unique<GraphNode>("cache-parent");
        auto child = std::make_unique<GraphNode>("cache-child");
        auto* childObserver = child.get();
        parent->addChild(std::move(child));

        childObserver->rotation();                          // prime the child's cache
        parent->setLocalEulerAngles(0.0f, 90.0f, 0.0f);     // only the PARENT changes
        passed &= expectRotationAxis(childObserver, Vector3(0.0f, 0.0f, -1.0f),
            "a parent rotation invalidates the child's cached world rotation");
    }

    // Removing a child leaves a hole that children() closes in order (2026-10-01); it
    // used to find and erase, a scan and a shift per removal. The order of the rest, the
    // removal of a child that is no longer there, a child deleted while still attached,
    // and a removal made while walking the parent are what must not change.
    {
        auto parent = std::make_unique<GraphNode>("holes-parent");
        std::vector<GraphNode*> kids;
        for (int i = 0; i < 6; ++i) {
            auto child = std::make_unique<GraphNode>("kid" + std::to_string(i));
            kids.push_back(child.get());
            parent->addChild(std::move(child));
        }
        const auto names = [&parent] {
            std::string joined;
            for (const auto& child : parent->children()) {
                joined += child ? child->name() + " " : std::string("null ");
            }
            return joined;
        };

        auto removed = parent->removeChild(kids[2]);
        auto removedFront = parent->removeChild(kids[0]);
        passed &= expect(removed.get() == kids[2] && removedFront.get() == kids[0] && !kids[2]->parent(),
            "removeChild hands back the child, detached");
        passed &= expect(names() == "kid1 kid3 kid4 kid5 ", "the others keep their order");
        passed &= expect(parent->findByName("kid4") == kids[4], "and are still found");
        passed &= expect(parent->removeChild(kids[2]) == nullptr, "removing it again finds nothing");

        auto removedBack = parent->removeChild(kids[5]);
        auto late = std::make_unique<GraphNode>("late");
        GraphNode* lateRaw = late.get();
        parent->addChild(std::move(late));
        auto removedMiddle = parent->removeChild(kids[3]);
        passed &= expect(names() == "kid1 kid4 late ", "removals either side of a later addition");
        passed &= expect(parent->removeChild(lateRaw).get() == lateRaw && names() == "kid1 kid4 ",
            "and the later child is removed by its own slot after a compaction");

        delete kids[1];   // still attached: the destructor detaches it from its parent
        passed &= expect(names() == "kid4 ", "a child deleted while attached leaves the rest in order");

        // A removal during a walk shows as a hole under the walk, never as a shift.
        for (int i = 0; i < 3; ++i) {
            parent->addChild(std::make_unique<GraphNode>("walk" + std::to_string(i)));
        }
        const auto& walked = parent->children();
        std::vector<std::unique_ptr<GraphNode>> taken;
        std::string visited;
        for (size_t i = 0; i < walked.size(); ++i) {
            if (!walked[i]) {
                visited += "hole ";
                continue;
            }
            visited += walked[i]->name() + " ";
            if (walked[i]->name() == "kid4") {
                taken.push_back(parent->removeChild(parent->findByName("walk1")));
            }
        }
        passed &= expect(visited == "kid4 walk0 hole walk2 ", "a walk sees the removed sibling as a hole");
        passed &= expect(names() == "kid4 walk0 walk2 ", "which the next read closes");
    }

    return passed ? 0 : 1;
}
