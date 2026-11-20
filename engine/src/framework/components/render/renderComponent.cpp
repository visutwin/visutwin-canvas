// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.02.2026
//
#include "renderComponent.h"

#include "framework/components/componentRegistry.h"
#include "primitiveGeometry.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "framework/batching/batchManager.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "scene/skinInstance.h"

namespace visutwin::canvas
{
    namespace
    {
        /**
         * The mesh of one primitive type on one device, SHARED by every render component
         * of that type: a scene of ten thousand boxes has
         * one box mesh, not ten thousand, and draws of one material and one primitive
         * then keep one vertex buffer bound. A mesh per component would repeat the
         * geometry, tangents and GPU buffers (about 190 KB of GPU memory for a sphere)
         * and dominate the cost of creating a primitive entity.
         *
         * DEVIATION: held WEAKLY. Upstream keeps each primitive for the device's
         * lifetime; here the components that use a mesh co-own it (`_ownedMeshes`) and
         * the cache only finds a live one, so the last component to go frees it. The
         * device fires no "destroy" event a strong cache could clear itself on, and a
         * mesh freed at static destruction would release its buffers into a device that
         * is already gone.
         *
         * Nothing may modify a mesh reached through a primitive component: it is every
         * such component's mesh. Per-instance state (material, lightmap, mask, stencil)
         * lives on the MeshInstance.
         */
        std::shared_ptr<Mesh> sharedPrimitiveMesh(const std::shared_ptr<GraphicsDevice>& device,
            const std::string& type)
        {
            static std::map<std::pair<const GraphicsDevice*, std::string>, std::weak_ptr<Mesh>> cache;
            std::weak_ptr<Mesh>& slot = cache[{device.get(), type}];
            if (auto mesh = slot.lock()) {
                return mesh;
            }

            PrimitiveGeometry geometry;
            if (type == "box") {
                geometry = createBoxGeometry();
            } else if (type == "sphere") {
                geometry = createSphereGeometry();
            } else if (type == "cylinder") {
                geometry = createCylinderGeometry();
            } else if (type == "cone") {
                geometry = createConeGeometry();
            } else if (type == "capsule") {
                geometry = createCapsuleGeometry();
            } else if (type == "plane") {
                geometry = createPlaneGeometry();
            } else if (type == "torus") {
                geometry = createTorusGeometry();
            } else {
                spdlog::warn("Unsupported render primitive type '{}'", type);
                return nullptr;
            }

            auto mesh = createMeshFromGeometry(device, geometry);
            slot = mesh;
            return mesh;
        }
    }

    RenderComponent::RenderComponent(IComponentSystem* system, Entity* entity)
        : Component(system, entity), _type("asset")
    {
        listInstance(this);
    }

    RenderComponent::~RenderComponent()
    {
        // A batch still pointing at these mesh instances must go before they do.
        if (auto* batches = batcher(); batches && !_meshInstances.empty()) {
            batches->sourcesLeaving(_batchGroupId);
        }
        clearMeshInstances();
        _ownedMeshes.clear();

        unlistInstance();
    }

    const std::vector<MeshInstance*>& RenderComponent::meshInstances() const
    {
        rebuildMeshInstanceView();
        return _meshInstanceView;
    }

    MeshInstance* RenderComponent::addMeshInstance(std::unique_ptr<MeshInstance> meshInstance)
    {
        if (!meshInstance) {
            return nullptr;
        }

        auto* meshInstanceRaw = meshInstance.get();
        // The component's clipping replaces the instance's own, also when there is none.
        meshInstanceRaw->setClipping(_clip);
        meshInstanceRaw->setRenderStyle(_renderStyle);
        if (_customAabb) {
            meshInstanceRaw->setLocalCustomAabb(&*_customAabb);
        }
        _meshInstances.push_back(std::move(meshInstance));
        _meshInstanceViewDirty = true;
        CullVersion::bump();
        // A new source for the group.
        if (auto* batches = batcher(); batches && active()) {
            batches->markGroupDirty(_batchGroupId);
        }
        return meshInstanceRaw;
    }

    BatchManager* RenderComponent::batcher() const
    {
        if (_batchGroupId < 0 || entity() == nullptr || entity()->engine() == nullptr) {
            return nullptr;
        }
        return entity()->engine()->batcher();
    }

    void RenderComponent::setBatchGroupId(const int id)
    {
        if (id == _batchGroupId) {
            return;
        }
        if (auto* batches = batcher(); batches && active()) {
            batches->sourcesLeaving(_batchGroupId);
        }
        _batchGroupId = id;
        for (const auto& mi : _meshInstances) {
            mi->setBatchGroupId(id);
        }
        if (auto* batches = batcher(); batches && active()) {
            batches->markGroupDirty(_batchGroupId);
        }
    }

    // The render component inserts into and removes from the batcher on
    // enable and disable. Disabling is also what an entity's destroy() does first.
    void RenderComponent::onEnable()
    {
        CullVersion::bump();
        if (auto* batches = batcher()) {
            batches->markGroupDirty(_batchGroupId);
        }
    }

    void RenderComponent::onDisable()
    {
        CullVersion::bump();
        if (auto* batches = batcher()) {
            batches->sourcesLeaving(_batchGroupId);
        }
    }

    bool RenderComponent::removeMeshInstance(const MeshInstance* meshInstance)
    {
        const auto it = std::find_if(_meshInstances.begin(), _meshInstances.end(),
            [meshInstance](const std::unique_ptr<MeshInstance>& owned) { return owned.get() == meshInstance; });
        if (meshInstance == nullptr || it == _meshInstances.end()) {
            return false;
        }
        // A batch keeps raw pointers to its sources, so the group goes before the instance.
        if (auto* batches = batcher()) {
            batches->sourcesLeaving(_batchGroupId);
        }
        _meshInstances.erase(it);
        _meshInstanceViewDirty = true;
        CullVersion::bump();
        return true;
    }

    void RenderComponent::clearMeshInstances()
    {
        if (auto* batches = batcher(); batches && !_meshInstances.empty()) {
            batches->sourcesLeaving(_batchGroupId);
        }
        _meshInstances.clear();
        _meshInstanceView.clear();
        _meshInstanceViewDirty = false;
        CullVersion::bump();
    }

    void RenderComponent::setType(const std::string& type)
    {
        if (_type == type) {
            return;
        }
        _type = type;
        rebuildPrimitiveMesh();
    }

    void RenderComponent::setMaterial(Material* material)
    {
        if (_material == material) {
            return;
        }
        _material = material;

        // A primitive's instances take the new material; nothing is rebuilt.
        // Rebuilding the primitive would build a second mesh for the common setType,
        // setMaterial order and drop whatever had been set on the instance.
        if (_type != "asset" && !_meshInstances.empty()) {
            // A batch bakes its sources' material in, so its group is torn down and
            // rebuilt from them.
            if (auto* batches = batcher(); batches && active()) {
                batches->sourcesLeaving(_batchGroupId);
            }
            for (const auto& meshInstance : _meshInstances) {
                if (meshInstance) {
                    meshInstance->setMaterial(material);
                }
            }
        }
    }

    void RenderComponent::setReceiveShadows(const bool value)
    {
        _receiveShadows = value;
        //set receiveShadows() propagates to all mesh instances.
        for (const auto& mi : _meshInstances) {
            if (mi) {
                mi->setReceiveShadows(value);
            }
        }
    }

    void RenderComponent::setCastShadows(const bool value)
    {
        _castShadows = value;
        //set castShadows() propagates to all mesh instances.
        for (const auto& mi : _meshInstances) {
            if (mi) {
                mi->setCastShadows(value);
            }
        }
    }

    void RenderComponent::setCustomAabb(const std::optional<BoundingBox>& aabb)
    {
        _customAabb = aabb;
        for (const auto& mi : _meshInstances) {
            if (mi) {
                mi->setLocalCustomAabb(_customAabb ? &*_customAabb : nullptr);
            }
        }
    }

    void RenderComponent::setRenderStyle(const int style)
    {
        _renderStyle = style;
        for (const auto& mi : _meshInstances) {
            if (mi) {
                mi->setRenderStyle(style);
            }
        }
    }

    void RenderComponent::setClippingPlanes(const std::span<const Plane> planes)
    {
        _clip.setPlanes(planes);
        applyClipping();
    }

    void RenderComponent::setClipIntersection(const bool value)
    {
        if (_clip.intersection() != value) {
            _clip.setIntersection(value);
            applyClipping();
        }
    }

    void RenderComponent::applyClipping()
    {
        bool wasClipped = false;
        for (const auto& mi : _meshInstances) {
            wasClipped = wasClipped || (mi && mi->clipped());
        }
        // A batch keeps raw pointers to its sources and draws them with one block, so a
        // source that becomes clipped leaves its batch, and one no longer clipped joins.
        if (auto* batches = batcher(); batches && active() && wasClipped != !_clip.empty()) {
            batches->sourcesLeaving(_batchGroupId);
        }
        for (const auto& mi : _meshInstances) {
            if (mi) {
                mi->setClipping(_clip);
            }
        }
    }

    void RenderComponent::cloneFrom(const Component* source)
    {
        const auto* src = dynamic_cast<const RenderComponent*>(source);
        if (!src) {
            return;
        }

        // Fields, not setters: setType / setMaterial would build a SECOND primitive
        // mesh when the source's can be shared.
        _type = src->_type;
        _layers = src->_layers;
        _material = src->_material;
        _receiveShadows = src->_receiveShadows;
        _castShadows = src->_castShadows;
        _renderStyle = src->_renderStyle;
        _customAabb = src->_customAabb;
        _batchGroupId = src->_batchGroupId;
        _clip = src->_clip;
        setEnabled(src->enabled());

        // The mesh instances share the source's meshes and materials. A primitive's mesh is owned
        // by the component, so the clone co-owns it: it has to outlive the source.
        //
        // An instance another owner attached here — a splat (GSplatComponent), an
        // emitter (ParticleSystemComponent), a storage draw (WideLineRenderer) — is
        // NOT copied: its owner builds the clone's own, and may already have done so
        // on this component, which is why nothing is cleared first.
        _ownedMeshes = src->_ownedMeshes;
        for (const auto& srcMi : src->_meshInstances) {
            if (!srcMi || srcMi->gsplatInstance() || srcMi->particleEmitter() ||
                srcMi->storageDrawCount() > 0) {
                continue;
            }
            addMeshInstance(srcMi->cloneFor(_entity));
        }
    }

    void RenderComponent::resolveClonedReferences(const Component* /*source*/, const CloneNodeMap& map)
    {
        // A skinned mesh's bones are nodes of the model it came with; the clone must
        // be driven by the CLONED skeleton. A bone outside the cloned subtree stays shared.
        for (const auto& mi : _meshInstances) {
            auto* skin = mi ? mi->skinInstance() : nullptr;
            if (!skin) {
                continue;
            }
            std::vector<GraphNode*> bones = skin->bones();
            for (auto& bone : bones) {
                bone = remapCloned(bone, map);
            }
            skin->setBones(std::move(bones));
            skin->setRootBone(remapCloned(skin->rootBone(), map));
        }
    }

    void RenderComponent::rebuildPrimitiveMesh()
    {
        clearMeshInstances();
        _ownedMeshes.clear();

        if (_type == "asset") {
            return;
        }

        auto* owner = entity();
        auto* ownerEngine = owner ? owner->engine() : nullptr;
        const auto device = ownerEngine ? ownerEngine->graphicsDevice() : nullptr;
        if (!device) {
            return;
        }

        auto mesh = sharedPrimitiveMesh(device, _type);
        if (!mesh) {
            return;
        }

        _ownedMeshes.push_back(mesh);
        auto meshInstance = std::make_unique<MeshInstance>(mesh.get(), _material, owner);
        meshInstance->setReceiveShadows(_receiveShadows);
        meshInstance->setCastShadows(_castShadows);
        meshInstance->setBatchGroupId(_batchGroupId);
        addMeshInstance(std::move(meshInstance));
    }

    void RenderComponent::rebuildMeshInstanceView() const
    {
        if (!_meshInstanceViewDirty) {
            return;
        }

        _meshInstanceView.clear();
        _meshInstanceView.reserve(_meshInstances.size());
        for (const auto& meshInstance : _meshInstances) {
            _meshInstanceView.push_back(meshInstance.get());
        }
        _meshInstanceViewDirty = false;
    }
}
