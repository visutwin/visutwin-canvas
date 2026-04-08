// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 10.09.2025.
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

        /**
         *
         * Clones mesh instances (sharing mesh/material), layers, type, shadow flags.
         */
        void cloneFrom(const Component* source) override;

        //batchGroupId getter/setter.
        // Propagates to all MeshInstances.
        int batchGroupId() const { return _batchGroupId; }
        void setBatchGroupId(int id) {
            _batchGroupId = id;
            for (const auto& mi : _meshInstances) {
                mi->setBatchGroupId(id);
            }
        }

        static const std::vector<RenderComponent*>& instances() { return _instances; }

    private:
        void rebuildPrimitiveMesh();
        void rebuildMeshInstanceView() const;

        inline static std::vector<RenderComponent*> _instances;

        std::vector<std::unique_ptr<MeshInstance>> _meshInstances;
        mutable std::vector<MeshInstance*> _meshInstanceView;
        mutable bool _meshInstanceViewDirty = true;
        std::vector<std::shared_ptr<Mesh>> _ownedMeshes;
        std::string _type;
        Material* _material = nullptr;

        bool _receiveShadows = true;
        bool _castShadows = true;
        int _batchGroupId = -1;  // BatchGroup::NOID

        // RenderComponent defaults to WORLD layer membership.
        // Intentional temporary deviation: WORLD layer id is currently 1 in this C++ bootstrap.
        std::vector<int> _layers = {1};
    };
}
