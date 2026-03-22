// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#pragma once

#include <optional>
#include <unordered_map>
#include <vector>

#include "core/math/vector3.h"
#include "core/shape/boundingSphere.h"

namespace visutwin::canvas
{
    class CameraComponent;
    class Engine;
    class MeshInstance;
    class Scene;

    class Picker
    {
    public:
        Picker(Engine* app, int width, int height, bool depth = false);

        void resize(int width, int height);

        void prepare(CameraComponent* camera, Scene* scene, const std::vector<int>& layers = {});

        std::vector<MeshInstance*> getSelection(int x, int y, int width = 1, int height = 1) const;
        MeshInstance* getSelectionSingle(int x, int y) const;
        std::optional<Vector3> getWorldPoint(int x, int y) const;

        int width() const { return _width; }
        int height() const { return _height; }

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

        struct Rect
        {
            int x = 0;
            int y = 0;
            int width = 1;
            int height = 1;
        };

        bool projectPoint(const Vector3& worldPos, float& outX, float& outY) const;
        bool buildRay(int x, int y, Vector3& outOrigin, Vector3& outDirection) const;
        Rect sanitizeRect(int x, int y, int width, int height) const;
        bool isLayerAllowed(const std::vector<int>& objectLayers) const;

        Engine* _app = nullptr;
        CameraComponent* _camera = nullptr;
        Scene* _scene = nullptr;
        bool _depth = false;
        int _width = 1;
        int _height = 1;
        std::vector<int> _layers;
        std::vector<Candidate> _candidates;
        std::unordered_map<MeshInstance*, size_t> _candidateIndex;
    };
}
