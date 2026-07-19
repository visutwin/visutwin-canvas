// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
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

    return passed ? 0 : 1;
}
