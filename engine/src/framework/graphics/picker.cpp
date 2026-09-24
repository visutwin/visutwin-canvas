// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
//
#include "picker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/shape/ray.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "scene/meshInstance.h"

namespace visutwin::canvas
{
    // `app` mirrors upstream's constructor; this CPU picker needs nothing from it.
    Picker::Picker(Engine* /*app*/, const int width, const int height, const bool depth)
        : _depth(depth)
    {
        resize(width, height);
    }

    void Picker::resize(const int width, const int height)
    {
        _width = std::max(1, width);
        _height = std::max(1, height);
    }

    void Picker::prepare(CameraComponent* camera, Scene* scene, const std::vector<int>& layers)
    {
        _camera = camera;
        _scene = scene;
        _layers = layers.empty() ? (_camera ? _camera->layers() : std::vector<int>{}) : layers;
        _candidates.clear();
        _candidateIndex.clear();

        if (!_camera || !_camera->camera() || !_camera->entity()) {
            return;
        }

        const Vector3 cameraPos = _camera->entity()->position();

        for (auto* renderComponent : RenderComponent::instances()) {
            if (!renderComponent || !renderComponent->enabled()) {
                continue;
            }

            if (!isLayerAllowed(renderComponent->layers())) {
                continue;
            }

            for (auto* meshInstance : renderComponent->meshInstances()) {
                if (!meshInstance || !meshInstance->node()) {
                    continue;
                }

                const BoundingBox aabb = meshInstance->aabb();
                const Vector3 center = aabb.center();
                const Vector3 half = aabb.halfExtents();

                std::array<Vector3, 8> corners = {{
                    center + half * Vector3(-1.0f, -1.0f, -1.0f),
                    center + half * Vector3( 1.0f, -1.0f, -1.0f),
                    center + half * Vector3(-1.0f,  1.0f, -1.0f),
                    center + half * Vector3( 1.0f,  1.0f, -1.0f),
                    center + half * Vector3(-1.0f, -1.0f,  1.0f),
                    center + half * Vector3( 1.0f, -1.0f,  1.0f),
                    center + half * Vector3(-1.0f,  1.0f,  1.0f),
                    center + half * Vector3( 1.0f,  1.0f,  1.0f)
                }};

                bool anyProjected = false;
                float minX = std::numeric_limits<float>::max();
                float minY = std::numeric_limits<float>::max();
                float maxX = std::numeric_limits<float>::lowest();
                float maxY = std::numeric_limits<float>::lowest();

                for (const auto& corner : corners) {
                    float sx = 0.0f;
                    float sy = 0.0f;
                    if (!projectPoint(corner, sx, sy)) {
                        continue;
                    }
                    anyProjected = true;
                    minX = std::min(minX, sx);
                    minY = std::min(minY, sy);
                    maxX = std::max(maxX, sx);
                    maxY = std::max(maxY, sy);
                }

                if (!anyProjected) {
                    continue;
                }

                Candidate candidate;
                candidate.meshInstance = meshInstance;
                candidate.minX = minX;
                candidate.minY = minY;
                candidate.maxX = maxX;
                candidate.maxY = maxY;
                const Vector3 delta = center - cameraPos;
                candidate.distanceSq = delta.lengthSquared();
                candidate.bounds = BoundingSphere(center, std::max(half.length(), 0.001f));

                _candidateIndex[candidate.meshInstance] = _candidates.size();
                _candidates.push_back(candidate);
            }
        }
    }

    std::vector<MeshInstance*> Picker::getSelection(const int x, const int y, const int width, const int height) const
    {
        std::vector<MeshInstance*> selection;
        if (!_camera) {
            return selection;
        }

        const Rect rect = sanitizeRect(x, y, width, height);
        const float rectMaxX = static_cast<float>(rect.x + rect.width);
        const float rectMaxY = static_cast<float>(rect.y + rect.height);

        std::unordered_set<MeshInstance*> seen;
        for (const auto& candidate : _candidates) {
            if (!candidate.meshInstance) {
                continue;
            }

            if (candidate.maxX < static_cast<float>(rect.x) || candidate.minX > rectMaxX ||
                candidate.maxY < static_cast<float>(rect.y) || candidate.minY > rectMaxY) {
                continue;
            }

            if (seen.insert(candidate.meshInstance).second) {
                selection.push_back(candidate.meshInstance);
            }
        }

        std::sort(selection.begin(), selection.end(), [&](const MeshInstance* a, const MeshInstance* b) {
            const auto ita = _candidateIndex.find(const_cast<MeshInstance*>(a));
            const auto itb = _candidateIndex.find(const_cast<MeshInstance*>(b));
            if (ita == _candidateIndex.end() || itb == _candidateIndex.end()) {
                return a < b;
            }
            return _candidates[ita->second].distanceSq < _candidates[itb->second].distanceSq;
        });

        return selection;
    }

    MeshInstance* Picker::getSelectionSingle(const int x, const int y) const
    {
        const auto selection = getSelection(x, y, 1, 1);
        return selection.empty() ? nullptr : selection.front();
    }

    // DEVIATION: upstream reads the pick buffer's DEPTH and unprojects it, so its point
    // is on the rendered surface. This picker has no GPU readback; it intersects the
    // pixel's ray with each candidate's bounding SPHERE (around its world AABB), so the
    // point is on that sphere's camera-facing side, not on the mesh.
    std::optional<Vector3> Picker::getWorldPoint(const int x, const int y) const
    {
        if (!_depth || !_camera) {
            return std::nullopt;
        }

        Vector3 rayOrigin;
        Vector3 rayDirection;
        if (!buildRay(x, y, rayOrigin, rayDirection)) {
            return std::nullopt;
        }

        const auto selected = getSelection(x, y, 1, 1);
        if (selected.empty()) {
            return std::nullopt;
        }

        Ray ray(rayOrigin, rayDirection);
        float nearestDistanceSq = std::numeric_limits<float>::max();
        std::optional<Vector3> nearestPoint;

        for (auto* meshInstance : selected) {
            const auto it = _candidateIndex.find(meshInstance);
            if (it == _candidateIndex.end()) {
                continue;
            }

            Vector3 hitPoint;
            if (!_candidates[it->second].bounds.intersectsRay(ray, &hitPoint)) {
                continue;
            }

            const float hitDistanceSq = (hitPoint - rayOrigin).lengthSquared();
            if (hitDistanceSq < nearestDistanceSq) {
                nearestDistanceSq = hitDistanceSq;
                nearestPoint = hitPoint;
            }
        }

        return nearestPoint;
    }

    bool Picker::projectPoint(const Vector3& worldPos, float& outX, float& outY) const
    {
        if (!_camera || !_camera->camera() || !_camera->entity()) {
            return false;
        }

        const Matrix4 viewMatrix = _camera->entity()->worldTransform().inverse();
        const Matrix4& projMatrix = _camera->camera()->projectionMatrix();

        const Vector3 viewPos = viewMatrix.transformPoint(worldPos);
        if (viewPos.getZ() >= 0.0f) {
            return false;
        }

        const Vector4 clipPos = projMatrix * Vector4(viewPos, 1.0f);
        if (std::abs(clipPos.getW()) < 1e-6f) {
            return false;
        }

        const Vector3 ndc = clipPos.perspectiveDivide();
        const float ndcX = ndc.getX();
        const float ndcY = ndc.getY();

        outX = (ndcX * 0.5f + 0.5f) * static_cast<float>(_width);
        outY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(_height);
        return true;
    }

    bool Picker::buildRay(const int x, const int y, Vector3& outOrigin, Vector3& outDirection) const
    {
        if (!_camera || !_camera->camera() || !_camera->entity()) {
            return false;
        }

        // Through the CENTRE of the pixel the pick reads (upstream 5cc6269d5), after
        // clamping it into the buffer. Through the integer coordinate the ray passed the
        // pixel's top-left corner, so a picked point sat half a pixel off the surface
        // point the pixel shows — two canvas pixels at a 0.25 pick scale.
        const Rect rect = sanitizeRect(x, y, 1, 1);
        const float px = static_cast<float>(rect.x) + 0.5f;
        const float py = static_cast<float>(rect.y) + 0.5f;

        const float ndcX = (px / static_cast<float>(_width)) * 2.0f - 1.0f;
        const float ndcY = 1.0f - (py / static_cast<float>(_height)) * 2.0f;

        const Matrix4 viewMatrix = _camera->entity()->worldTransform().inverse();
        const Matrix4 viewProjection = _camera->camera()->projectionMatrix() * viewMatrix;
        const Matrix4 invViewProjection = viewProjection.inverse();

        // The camera projection is GL-style, NDC z from -1 (near) to +1 (far). This
        // used +1 as "near" and 0 as "far", so the ray started at the far plane and
        // pointed back at the camera: the nearest hit was the FAR side of the object.
        Vector4 nearClip(ndcX, ndcY, -1.0f, 1.0f);
        Vector4 farClip(ndcX, ndcY, 1.0f, 1.0f);

        nearClip = invViewProjection * nearClip;
        farClip = invViewProjection * farClip;

        if (std::abs(nearClip.getW()) < 1e-6f || std::abs(farClip.getW()) < 1e-6f) {
            return false;
        }

        const Vector3 nearWorld = nearClip.perspectiveDivide();
        const Vector3 farWorld = farClip.perspectiveDivide();

        const Vector3 dir = farWorld - nearWorld;
        if (dir.lengthSquared() < 1e-10f) {
            return false;
        }

        outOrigin = nearWorld;
        outDirection = dir.normalized();
        return true;
    }

    Picker::Rect Picker::sanitizeRect(int x, int y, int width, int height) const
    {
        x = std::clamp(x, 0, std::max(0, _width - 1));
        y = std::clamp(y, 0, std::max(0, _height - 1));
        width = std::max(1, width);
        height = std::max(1, height);
        width = std::min(width, _width - x);
        height = std::min(height, _height - y);
        return Rect{ x, y, width, height };
    }

    bool Picker::isLayerAllowed(const std::vector<int>& objectLayers) const
    {
        if (_layers.empty()) {
            return true;
        }

        return std::any_of(objectLayers.begin(), objectLayers.end(), [&](const int layer) {
            return std::find(_layers.begin(), _layers.end(), layer) != _layers.end();
        });
    }
}
