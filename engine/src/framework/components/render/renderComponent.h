// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.09.2025
//
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <scene/meshInstance.h>
#include <scene/constants.h>

#include "framework/components/component.h"

namespace visutwin::canvas
{
    class BatchManager;

    struct RenderComponentData
    {
    };

    /**
     * The RenderComponent enables an Entity to render 3D meshes. The type property can
     * be set to one of several predefined shapes (such as box, sphere, cone and so on).
     * Alternatively, the component can be configured to manage an arbitrary array of
     * MeshInstances. These can either be created programmatically or loaded from an Asset.
     */
    class RenderComponent : public Component
    {
    public:
        RenderComponent(IComponentSystem* system, Entity* entity);
        ~RenderComponent() override;

        void initializeComponentData() override {};

        const std::vector<MeshInstance*>& meshInstances() const;
        MeshInstance* addMeshInstance(std::unique_ptr<MeshInstance> meshInstance);
        /// Destroys one mesh instance this component owns, found by address (never
        /// dereferenced); false when it owns no such instance.
        bool removeMeshInstance(const MeshInstance* meshInstance);
        void clearMeshInstances();

        const std::vector<int>& layers() const { return _layers; }
        void setLayers(const std::vector<int>& layers) { _layers = layers; }

        const std::string& type() const { return _type; }
        void setType(const std::string& type);

        Material* material() const { return _material; }
        void setMaterial(Material* material);

        //receiveShadows getter/setter.
        // Propagates to all MeshInstances.
        bool receiveShadows() const { return _receiveShadows; }
        void setReceiveShadows(bool value);

        bool castShadows() const { return _castShadows; }
        void setCastShadows(bool value);

        /// The render style (RenderStyle) of every mesh instance the component has and gets: a
        /// mesh instance added later takes it too (MeshInstance::setRenderStyle). A clone copies it.
        int renderStyle() const { return _renderStyle; }
        void setRenderStyle(int style);

        /**
         *
         * Clones mesh instances (sharing mesh/material), layers, type, shadow flags.
         */
        void cloneFrom(const Component* source) override;
        void resolveClonedReferences(const Component* source, const CloneNodeMap& map) override;

        /**
         * Planes that clip this entity's mesh instances, in world space (copied, at most
         * ClipPlanes::kMaxPlanes), and whether they clip by intersection; see
         * MeshInstance::setClippingPlanes. Every change, and every mesh instance the component
         * gets, hands the instances the component's planes and mode together, replacing any
         * they were given directly, also when the component has none. A clipped instance is
         * not batched: the batch group is rebuilt when the instances' clipping comes or goes.
         * A clone copies the planes.
         */
        void setClippingPlanes(std::span<const Plane> planes);
        const std::vector<Plane>& clippingPlanes() const { return _clip.planes(); }
        void setClipIntersection(bool value);
        bool clipIntersection() const { return _clip.intersection(); }

        //batchGroupId getter/setter.
        // Propagates to all MeshInstances.
        int batchGroupId() const { return _batchGroupId; }
        /// Leaving a group tears that group's batches down (they point at this
        /// component's mesh instances); joining one marks it for rebuilding.
        void setBatchGroupId(int id);

        void onEnable() override;
        void onDisable() override;


    private:
        void rebuildPrimitiveMesh();
        BatchManager* batcher() const;
        void rebuildMeshInstanceView() const;
        /// Hands every mesh instance the component's clipping, rebuilding the batch group
        /// when that changes whether any of them is clipped.
        void applyClipping();


        std::vector<std::unique_ptr<MeshInstance>> _meshInstances;
        mutable std::vector<MeshInstance*> _meshInstanceView;
        mutable bool _meshInstanceViewDirty = true;
        std::vector<std::shared_ptr<Mesh>> _ownedMeshes;
        std::string _type;
        Material* _material = nullptr;

        bool _receiveShadows = true;
        bool _castShadows = true;
        int _renderStyle = 0;  // RENDERSTYLE_SOLID
        int _batchGroupId = -1;  // BatchGroup::NOID
        ClipPlanes _clip;

        // RenderComponent defaults to WORLD layer membership.
        // Intentional temporary deviation: WORLD layer id is currently 1 in this C++ bootstrap.
        std::vector<int> _layers = {1};
    };
}
