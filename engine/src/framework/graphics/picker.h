// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// Picker: prepare() renders the chosen layers into an offscreen buffer with
// each mesh instance's id as its colour (and the depth beside it), and the queries read
// that buffer — so a selection is what is VISIBLE at those pixels, occlusion and alpha
// test included, and getWorldPoint is a point on the rendered surface.
//
// The id pass draws a private clone of each mesh instance (sharing its mesh, node, skin
// and morph targets) with a clone of its material set to the pick variant
// (Material::setPick, VT_FEATURE_PICK): after the material's own alpha test and dither,
// the fragment writes the id packed into rgb. The buffers are read back with
// Texture::read once per prepare().
//
// A device that cannot render or read the buffer back (no backend: a test stub) falls
// back to the bounds of the mesh instances: a selection by projected bounding box,
// nearest first, and getWorldPoint on the bounding sphere. DEVIATION, that fallback only.
// Instanced draws (MeshInstance::setInstancing) are not cloned, so they are not picked.
//
#pragma once

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "core/math/matrix4.h"
#include "core/math/vector3.h"
#include "core/shape/boundingSphere.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Engine;
    class Layer;
    class Material;
    class MeshInstance;
    class RenderTarget;
    class Scene;
    class Texture;

    class Picker
    {
    public:
        Picker(Engine* app, int width, int height, bool depth = false);
        ~Picker();

        Picker(const Picker&) = delete;
        Picker& operator=(const Picker&) = delete;

        void resize(int width, int height);

        void prepare(CameraComponent* camera, Scene* scene, const std::vector<int>& layers = {});

        std::vector<MeshInstance*> getSelection(int x, int y, int width = 1, int height = 1) const;
        MeshInstance* getSelectionSingle(int x, int y) const;
        std::optional<Vector3> getWorldPoint(int x, int y) const;

        int width() const { return _width; }
        int height() const { return _height; }

        /// True when the last prepare() read a rendered id buffer back, false when it
        /// fell back to bounds.
        bool usesIdBuffer() const { return _idBufferValid; }

    private:
        struct Candidate
        {
            MeshInstance* meshInstance = nullptr;
            float minX = 0.0f;
            float minY = 0.0f;
            float maxX = 0.0f;
            float maxY = 0.0f;
            float distanceSq = 0.0f;
            BoundingSphere bounds;
        };

        /// The id pass's stand-in for one mesh instance, kept across prepare() calls
        /// while the source still has the same mesh, node and material state.
        struct PickDraw
        {
            const void* mesh = nullptr;
            const void* node = nullptr;
            const Material* sourceMaterial = nullptr;
            uint64_t sourceVersion = 0;
            uint32_t id = 0;
            std::shared_ptr<Material> material;
            std::unique_ptr<MeshInstance> clone;
            bool used = false;
        };

        struct Rect
        {
            int x = 0;
            int y = 0;
            int width = 1;
            int height = 1;
        };

        void collectCandidates();
        bool renderIdBuffer();
        void ensureTargets();
        PickDraw& pickDrawFor(MeshInstance* source, uint32_t id);

        bool projectPoint(const Vector3& worldPos, float& outX, float& outY) const;
        bool buildRay(int x, int y, Vector3& outOrigin, Vector3& outDirection) const;
        Rect sanitizeRect(int x, int y, int width, int height) const;
        bool isLayerAllowed(const std::vector<int>& objectLayers) const;

        std::vector<MeshInstance*> boundsSelection(const Rect& rect) const;
        std::optional<Vector3> boundsWorldPoint(int x, int y) const;

        Engine* _app = nullptr;
        CameraComponent* _camera = nullptr;
        Scene* _scene = nullptr;
        bool _depth = false;
        int _width = 1;
        int _height = 1;
        std::vector<int> _layers;
        std::vector<Candidate> _candidates;
        std::unordered_map<MeshInstance*, size_t> _candidateIndex;

        // The id pass.
        std::shared_ptr<Texture> _colorBuffer;
        std::shared_ptr<Texture> _depthBuffer;
        std::shared_ptr<RenderTarget> _renderTarget;
        std::unique_ptr<Layer> _pickLayer;
        std::unordered_map<MeshInstance*, PickDraw> _pickDraws;
        std::vector<MeshInstance*> _idToMeshInstance;   // index = id - 1
        std::vector<uint8_t> _idPixels;                 // RGBA8, row 0 at the top
        std::vector<float> _depthPixels;                // [0, 1] window depth
        Matrix4 _inverseViewProjection = Matrix4::identity();
        bool _idBufferValid = false;
    };
}
