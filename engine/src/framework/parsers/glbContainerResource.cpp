// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 20.12.2025.
//
#include "glbContainerResource.h"

#include <spdlog/spdlog.h>

#include "framework/components/animation/animationComponent.h"
#include "framework/components/render/renderComponent.h"
#include "scene/morphInstance.h"
#include "scene/skinInstance.h"

namespace visutwin::canvas
{
    Entity* GlbContainerResource::instantiateRenderEntity()
    {
        auto* root = new Entity();

        // container resource instantiateRenderEntity() produces an entity hierarchy
        // with render components / mesh instances populated from parsed glTF payload and node transforms.
        if (_nodePayloads.empty()) {
            if (_meshPayloads.empty()) {
                return root;
            }

            // Fallback for payloads without node metadata.
            auto renderComponent = std::make_unique<RenderComponent>(nullptr, root);
            auto* renderComponentRaw = renderComponent.get();
                for (const auto& payload : _meshPayloads) {
                    if (!payload.mesh) {
                        continue;
                    }
                    // Shared ownership: instantiated entities must survive Asset::unload()
                    // deleting this container.
                    auto meshInstance = std::make_unique<MeshInstance>(payload.mesh, payload.material, root);
                    renderComponentRaw->addMeshInstance(std::move(meshInstance));
                }
            root->addComponentInstance(std::move(renderComponent), componentTypeID<RenderComponent>());
            return root;
        }

        std::vector<Entity*> nodeEntities(_nodePayloads.size(), nullptr);
        std::vector<std::vector<MeshInstance*>> nodeMeshInstances(_nodePayloads.size());
        for (size_t i = 0; i < _nodePayloads.size(); ++i) {
            const auto& nodePayload = _nodePayloads[i];
            if (nodePayload.skip) {
                continue;  // Consumed POINTS leaf — no entity created.
            }
            auto* nodeEntity = new Entity();
            if (!nodePayload.name.empty()) {
                nodeEntity->setName(nodePayload.name);
            }
            nodeEntity->setLocalPosition(
                nodePayload.translation.getX(),
                nodePayload.translation.getY(),
                nodePayload.translation.getZ()
            );
            nodeEntity->setLocalRotation(nodePayload.rotation);
            nodeEntity->setLocalScale(nodePayload.scale);

            if (!nodePayload.meshPayloadIndices.empty()) {
                auto renderComponent = std::make_unique<RenderComponent>(nullptr, nodeEntity);
                auto* renderComponentRaw = renderComponent.get();
                for (const auto meshPayloadIndex : nodePayload.meshPayloadIndices) {
                    if (meshPayloadIndex >= _meshPayloads.size()) {
                        continue;
                    }
                    const auto& meshPayload = _meshPayloads[meshPayloadIndex];
                    if (!meshPayload.mesh) {
                        continue;
                    }
                    auto meshInstance = std::make_unique<MeshInstance>(meshPayload.mesh, meshPayload.material, nodeEntity);
                    meshInstance->setCastShadow(meshPayload.castShadow);

                    // Morph targets: each mesh instance gets its own weight set.
                    if (meshPayload.morph) {
                        auto morphInstance = std::make_shared<MorphInstance>(meshPayload.morph);
                        for (size_t w = 0; w < meshPayload.morphInitialWeights.size(); ++w) {
                            morphInstance->setWeight(static_cast<int>(w), meshPayload.morphInitialWeights[w]);
                        }
                        meshInstance->setMorphInstance(morphInstance);
                    }

                    nodeMeshInstances[i].push_back(meshInstance.get());
                    renderComponentRaw->addMeshInstance(std::move(meshInstance));
                }
                nodeEntity->addComponentInstance(std::move(renderComponent), componentTypeID<RenderComponent>());
            }

            nodeEntities[i] = nodeEntity;
        }

        // Resolve skins: one SkinInstance per skin, shared by every mesh instance of
        // every node that references it. Bones resolve by glTF node index directly
        // (DEVIATION: upstream resolves by bone name via findByName).
        std::vector<std::shared_ptr<SkinInstance>> skinInstances(_skinPayloads.size());
        for (size_t i = 0; i < _nodePayloads.size(); ++i) {
            const auto& nodePayload = _nodePayloads[i];
            if (nodePayload.skinIndex < 0 ||
                nodePayload.skinIndex >= static_cast<int>(_skinPayloads.size()) ||
                nodeMeshInstances[i].empty()) {
                continue;
            }
            auto& skinInstance = skinInstances[static_cast<size_t>(nodePayload.skinIndex)];
            if (!skinInstance) {
                const auto& skinPayload = _skinPayloads[static_cast<size_t>(nodePayload.skinIndex)];
                skinInstance = std::make_shared<SkinInstance>(skinPayload.skin);
                std::vector<GraphNode*> bones;
                bones.reserve(skinPayload.jointNodeIndices.size());
                for (const int jointNodeIndex : skinPayload.jointNodeIndices) {
                    Entity* bone = (jointNodeIndex >= 0 &&
                                    jointNodeIndex < static_cast<int>(nodeEntities.size()))
                        ? nodeEntities[static_cast<size_t>(jointNodeIndex)] : nullptr;
                    if (!bone) {
                        spdlog::error("GLB skin: joint node {} missing in instantiated hierarchy — "
                            "falling back to root", jointNodeIndex);
                        bone = root;
                    }
                    bones.push_back(bone);
                }
                skinInstance->setBones(std::move(bones));
                skinInstance->setRootBone(root);
            }
            for (auto* meshInstance : nodeMeshInstances[i]) {
                meshInstance->setSkinInstance(skinInstance);
            }
        }

        for (size_t i = 0; i < _nodePayloads.size(); ++i) {
            auto* parent = nodeEntities[i];
            if (!parent) {
                continue;
            }
            for (const auto childIndex : _nodePayloads[i].children) {
                if (childIndex < 0 || childIndex >= static_cast<int>(nodeEntities.size())) {
                    continue;
                }
                auto* child = nodeEntities[static_cast<size_t>(childIndex)];
                if (child) {
                    parent->addChild(child);
                }
            }
        }

        if (_rootNodeIndices.empty()) {
            std::vector<bool> isChild(nodeEntities.size(), false);
            for (const auto& nodePayload : _nodePayloads) {
                for (const auto childIndex : nodePayload.children) {
                    if (childIndex >= 0 && childIndex < static_cast<int>(isChild.size())) {
                        isChild[static_cast<size_t>(childIndex)] = true;
                    }
                }
            }

            for (size_t i = 0; i < nodeEntities.size(); ++i) {
                auto* nodeEntity = nodeEntities[i];
                if (nodeEntity && !isChild[i]) {
                    root->addChild(nodeEntity);
                }
            }
        } else {
            for (const auto rootIndex : _rootNodeIndices) {
                if (rootIndex < 0 || rootIndex >= static_cast<int>(nodeEntities.size())) {
                    continue;
                }
                auto* nodeEntity = nodeEntities[static_cast<size_t>(rootIndex)];
                if (nodeEntity) {
                    root->addChild(nodeEntity);
                }
            }
        }

        // Attach AnimationComponent if the GLB contained animations.
        //attaches animations to the instantiated entity.
        if (!_animTracks.empty()) {
            auto animComponent = std::make_unique<AnimationComponent>(nullptr, root);
            for (const auto& [name, track] : _animTracks) {
                animComponent->addAnimation(name, track);
            }
            root->addComponentInstance(std::move(animComponent), componentTypeID<AnimationComponent>());
        }

        return root;
    }
}
