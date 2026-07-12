// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 20.12.2025.
//
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include <framework/handlers/containerResource.h>
#include "framework/anim/evaluator/animTrack.h"
#include "scene/mesh.h"
#include "scene/morph.h"
#include "scene/skin.h"
#include "scene/materials/material.h"

namespace visutwin::canvas
{
    class Texture;

    struct GlbMeshPayload
    {
        std::shared_ptr<Mesh> mesh;
        std::shared_ptr<Material> material;
        std::shared_ptr<Morph> morph;              // Morph targets (nullptr when none).
        std::vector<float> morphInitialWeights;    // glTF mesh.weights (may be empty).
        bool castShadow = true;  // Set false for point cloud meshes.
    };

    struct GlbSkinPayload
    {
        std::shared_ptr<Skin> skin;
        /// glTF joint node indices, resolved to instantiated entities (by index,
        /// not by name — DEVIATION from upstream's findByName resolution).
        std::vector<int> jointNodeIndices;
    };

    struct GlbNodePayload
    {
        std::string name;
        Vector3 translation = Vector3(0.0f, 0.0f, 0.0f);
        Quaternion rotation = Quaternion(0.0f, 0.0f, 0.0f, 1.0f);
        Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
        std::vector<size_t> meshPayloadIndices;
        std::vector<int> children;
        int skinIndex = -1;  // Index into skin payloads (glTF node.skin), -1 = unskinned.
        bool skip = false;  // When true, no Entity is created (e.g., consumed POINTS leaf).
    };

    /**
     * Container resource returned by the GlbParser. Implements the ContainerResource interface.
     */
    class GlbContainerResource : public ContainerResource
    {
    public:
        void addMeshPayload(const GlbMeshPayload& payload) { _meshPayloads.push_back(payload); }
        void addNodePayload(const GlbNodePayload& payload) { _nodePayloads.push_back(payload); }
        void addSkinPayload(const GlbSkinPayload& payload) { _skinPayloads.push_back(payload); }
        size_t skinPayloadCount() const { return _skinPayloads.size(); }
        void addRootNodeIndex(const int index) { _rootNodeIndices.push_back(index); }
        void addOwnedTexture(const std::shared_ptr<Texture>& texture) { _ownedTextures.push_back(texture); }

        void addAnimTrack(const std::string& name, const std::shared_ptr<AnimTrack>& track) { _animTracks[name] = track; }
        const std::unordered_map<std::string, std::shared_ptr<AnimTrack>>& animTracks() const { return _animTracks; }

        Entity* instantiateRenderEntity() override;

    private:
        std::vector<GlbMeshPayload> _meshPayloads;
        std::vector<GlbNodePayload> _nodePayloads;
        std::vector<GlbSkinPayload> _skinPayloads;
        std::vector<int> _rootNodeIndices;
        std::vector<std::shared_ptr<Texture>> _ownedTextures;
        std::unordered_map<std::string, std::shared_ptr<AnimTrack>> _animTracks;
    };
}
