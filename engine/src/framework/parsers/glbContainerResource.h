// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers 20.12.2025.
//
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/color.h"
#include "core/math/quaternion.h"
#include "core/math/vector3.h"
#include <framework/handlers/containerResource.h>
#include "framework/anim/evaluator/animTrack.h"
#include "scene/materials/material.h"
#include "scene/mesh.h"
#include "scene/morph.h"
#include "scene/camera.h"
#include "scene/constants.h"
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

    /// A glTF camera (core glTF), in the units the camera component takes.
    struct GlbCameraPayload
    {
        ProjectionType projection = ProjectionType::Perspective;
        float nearClip = 0.1f;
        std::optional<float> farClip;        // absent: an infinite perspective; keep the default
        float fovDegrees = 45.0f;            // vertical, perspective only
        float orthoHeight = 10.0f;           // glTF ymag, the HALF height
        std::optional<float> aspectRatio;    // manual aspect when the file gives one
    };

    /// A KHR_lights_punctual light, in the units the light component takes.
    struct GlbLightPayload
    {
        LightType type = LightType::LIGHTTYPE_OMNI;
        Color color = Color(1.0f, 1.0f, 1.0f, 1.0f);
        float intensity = 1.0f;              // clamped to [0, 2], for non-physical scenes
        float luminance = 0.0f;              // the file's intensity x the unit conversion
        float range = 9999.0f;
        float innerConeDegrees = 0.0f;
        float outerConeDegrees = 45.0f;
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
        std::optional<GlbCameraPayload> camera;
        std::optional<GlbLightPayload> light;
    };

    /**
     * Container resource returned by the GlbParser. Implements the ContainerResource interface.
     */
    class GlbContainerResource : public ContainerResource
    {
    public:
        // A payload's material holds raw Texture*s into this container's textures, so
        // it keeps the container's texture list alive (Material::retainResource). A
        // mesh instance co-owns its mesh and material already; with this an entity
        // built from the container can outlive the Asset's unload() without its
        // materials pointing at freed textures, which is what happened until 2026-09-24.
        void addMeshPayload(const GlbMeshPayload& payload)
        {
            if (payload.material) {
                payload.material->retainResource(_ownedTextures);
            }
            _meshPayloads.push_back(payload);
        }
        void addNodePayload(const GlbNodePayload& payload) { _nodePayloads.push_back(payload); }
        void addSkinPayload(const GlbSkinPayload& payload) { _skinPayloads.push_back(payload); }
        size_t skinPayloadCount() const { return _skinPayloads.size(); }
        void addRootNodeIndex(const int index) { _rootNodeIndices.push_back(index); }
        void addOwnedTexture(const std::shared_ptr<Texture>& texture) { _ownedTextures->push_back(texture); }

        void addAnimTrack(const std::string& name, const std::shared_ptr<AnimTrack>& track) { _animTracks[name] = track; }
        const std::unordered_map<std::string, std::shared_ptr<AnimTrack>>& animTracks() const { return _animTracks; }

        /// The mesh payloads in the order the parser added them; tests read the parsed geometry through here.
        const std::vector<GlbMeshPayload>& meshPayloads() const { return _meshPayloads; }
        const std::vector<GlbNodePayload>& nodePayloads() const { return _nodePayloads; }
        const std::vector<int>& rootNodeIndices() const { return _rootNodeIndices; }
        /// The skin payloads in glTF skin order; tests read the bone bounds through here.
        const std::vector<GlbSkinPayload>& skinPayloads() const { return _skinPayloads; }

        Entity* instantiateRenderEntity() override;

    private:
        std::vector<GlbMeshPayload> _meshPayloads;
        std::vector<GlbNodePayload> _nodePayloads;
        std::vector<GlbSkinPayload> _skinPayloads;
        std::vector<int> _rootNodeIndices;
        // Shared, so the materials handed out can keep it alive past this container.
        std::shared_ptr<std::vector<std::shared_ptr<Texture>>> _ownedTextures =
            std::make_shared<std::vector<std::shared_ptr<Texture>>>();
        std::unordered_map<std::string, std::shared_ptr<AnimTrack>> _animTracks;
    };
}
