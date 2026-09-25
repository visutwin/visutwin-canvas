// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 20.12.2025.
//
#include "glbContainerResource.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "framework/components/animation/animationComponent.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/gsplat/gsplatComponent.h"
#include "framework/components/light/lightComponent.h"
#include "framework/components/render/renderComponent.h"
#include "scene/morphInstance.h"
#include "scene/skinInstance.h"

namespace visutwin::canvas
{
    Entity* GlbContainerResource::instantiateRenderEntity()
    {
        // container resource instantiateRenderEntity() produces an entity hierarchy
        // with render components / mesh instances populated from parsed glTF payload and node transforms.
        if (_nodePayloads.empty()) {
            auto* root = new Entity();
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

                    // EXT_mesh_gpu_instancing: every primitive of the node draws once per
                    // instance matrix, each composed with the node's own transform.
                    if (nodePayload.instanceBuffer && nodePayload.instanceCount > 0) {
                        meshInstance->setInstancing(nodePayload.instanceBuffer, nodePayload.instanceCount);
                    }

                    nodeMeshInstances[i].push_back(meshInstance.get());
                    renderComponentRaw->addMeshInstance(std::move(meshInstance));
                }
                nodeEntity->addComponentInstance(std::move(renderComponent), componentTypeID<RenderComponent>());
            }

            // A camera goes on the node itself: glTF and this engine both look down -Z.
            // Imported DISABLED, as upstream's createCamera does — a model does not
            // get to take over the view; the app enables the one it wants.
            if (nodePayload.camera) {
                const auto& payload = *nodePayload.camera;
                auto cameraComponent = std::make_unique<CameraComponent>(nullptr, nodeEntity);
                cameraComponent->initializeComponentData();
                Camera* camera = cameraComponent->camera();
                camera->setProjection(payload.projection);
                camera->setNearClip(payload.nearClip);
                if (payload.farClip) {
                    camera->setFarClip(*payload.farClip);
                }
                camera->setFov(payload.fovDegrees);
                camera->setOrthoHeight(payload.orthoHeight);
                if (payload.aspectRatio) {
                    camera->setAspectRatioMode(AspectRatioMode::ASPECT_MANUAL);
                    camera->setAspectRatio(*payload.aspectRatio);
                }
                cameraComponent->setEnabled(false);
                nodeEntity->addComponentInstance(std::move(cameraComponent), componentTypeID<CameraComponent>());
            }

            // A light goes on a CHILD turned 90 degrees about X: a glTF light shines down
            // its node's -Z, a light here down -Y (upstream adds the same extra entity,
            // named after the node). Imported DISABLED, as upstream's createLight does.
            if (nodePayload.light) {
                const auto& payload = *nodePayload.light;
                auto* lightEntity = new Entity();
                lightEntity->setName(nodeEntity->name());
                lightEntity->rotateLocal(90.0f, 0.0f, 0.0f);
                auto lightComponent = std::make_unique<LightComponent>(nullptr, lightEntity);
                lightComponent->setType(payload.type);
                lightComponent->setColor(payload.color);
                lightComponent->setIntensity(payload.intensity);
                lightComponent->setLuminance(payload.luminance);
                lightComponent->setRange(payload.range);
                lightComponent->setFalloffMode(LightFalloff::LIGHTFALLOFF_INVERSESQUARED);
                lightComponent->setInnerConeAngle(payload.innerConeDegrees);
                lightComponent->setOuterConeAngle(payload.outerConeDegrees);
                lightComponent->setEnabled(false);
                lightEntity->addComponentInstance(std::move(lightComponent), componentTypeID<LightComponent>());
                nodeEntity->addChild(lightEntity);
            }

            // KHR_gaussian_splatting: the first splat set on the node's entity, each
            // further one on a child named after it (upstream's layout).
            for (size_t splat = 0; splat < nodePayload.splats.size(); ++splat) {
                Entity* target = nodeEntity;
                if (splat > 0) {
                    target = new Entity();
                    target->setName(nodeEntity->name() + "_gsplat_" + std::to_string(splat));
                    nodeEntity->addChild(target);
                }
                auto gsplat = std::make_unique<GSplatComponent>(nullptr, target);
                auto* gsplatRaw = static_cast<GSplatComponent*>(
                    target->addComponentInstance(std::move(gsplat), componentTypeID<GSplatComponent>()));
                gsplatRaw->setResource(nodePayload.splats[splat]);
            }

            nodeEntities[i] = nodeEntity;
        }

        // Collect the scene's root nodes (explicit list when the parser recorded one,
        // otherwise every node nothing else parents).
        std::vector<Entity*> sceneRoots;
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
                if (nodeEntities[i] && !isChild[i]) {
                    sceneRoots.push_back(nodeEntities[i]);
                }
            }
        } else {
            for (const auto rootIndex : _rootNodeIndices) {
                if (rootIndex >= 0 && rootIndex < static_cast<int>(nodeEntities.size()) &&
                    nodeEntities[static_cast<size_t>(rootIndex)]) {
                    sceneRoots.push_back(nodeEntities[static_cast<size_t>(rootIndex)]);
                }
            }
        }

        // A scene with a single root node IS the returned hierarchy root — no wrapper
        // entity (upstream glb-parser.js `createScenes`). This matters to callers:
        // setLocalScale() on the result then REPLACES the root node's own scale the way
        // it does upstream, instead of multiplying with it. Only multi-root scenes (and
        // the node-less fallback above) get a wrapper.
        Entity* root = sceneRoots.size() == 1 ? sceneRoots.front() : new Entity();

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

        // Multi-root scenes hang every root node off the wrapper; a single-root scene
        // already IS `root`.
        if (sceneRoots.size() != 1) {
            for (auto* sceneRoot : sceneRoots) {
                root->addChild(sceneRoot);
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

    bool GlbContainerResource::applyMaterialVariantInstances(const std::vector<MeshInstance*>& instances,
        const std::string& name) const
    {
        int variant = -1;
        if (!name.empty()) {
            const auto it = std::find(_variantNames.begin(), _variantNames.end(), name);
            if (it == _variantNames.end()) {
                spdlog::warn("GlbContainerResource: no material variant named '{}'", name);
                return false;
            }
            variant = static_cast<int>(it - _variantNames.begin());
        }
        for (auto* instance : instances) {
            if (!instance || !instance->mesh()) {
                continue;
            }
            const auto payload = std::find_if(_meshPayloads.begin(), _meshPayloads.end(),
                [instance](const GlbMeshPayload& p) { return p.mesh.get() == instance->mesh(); });
            if (payload == _meshPayloads.end()) {
                continue;   // not built from this container
            }
            if (variant < 0) {
                // DEVIATION: upstream's reset (a null variant) assigns the engine's
                // default material; this puts the primitive's OWN material back.
                instance->setMaterial(payload->material);
            } else if (const auto mapped = payload->variantMaterials.find(variant);
                       mapped != payload->variantMaterials.end() && mapped->second) {
                instance->setMaterial(mapped->second);
            }
        }
        return true;
    }

    bool GlbContainerResource::applyMaterialVariant(Entity* entity, const std::string& name) const
    {
        if (!entity) {
            return false;
        }
        std::vector<MeshInstance*> instances;
        for (auto* render : entity->findComponents<RenderComponent>()) {
            const auto& list = render->meshInstances();
            instances.insert(instances.end(), list.begin(), list.end());
        }
        return applyMaterialVariantInstances(instances, name);
    }
}
