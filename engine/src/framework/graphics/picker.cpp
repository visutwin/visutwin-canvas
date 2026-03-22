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
    Picker::Picker(Engine* app, const int width, const int height, const bool depth)
        : _app(app), _depth(depth)
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
                    Vector3(center.getX() - half.getX(), center.getY() - half.getY(), center.getZ() - half.getZ()),
                    Vector3(center.getX() + half.getX(), center.getY() - half.getY(), center.getZ() - half.getZ()),
                    Vector3(center.getX() - half.getX(), center.getY() + half.getY(), center.getZ() - half.getZ()),
                    Vector3(center.getX() + half.getX(), center.getY() + half.getY(), center.getZ() - half.getZ()),
                    Vector3(center.getX() - half.getX(), center.getY() - half.getY(), center.getZ() + half.getZ()),
                    Vector3(center.getX() + half.getX(), center.getY() - half.getY(), center.getZ() + half.getZ()),
                    Vector3(center.getX() - half.getX(), center.getY() + half.getY(), center.getZ() + half.getZ()),
                    Vector3(center.getX() + half.getX(), center.getY() + half.getY(), center.getZ() + half.getZ())
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

        const Vector4 clipPos = projMatrix * Vector4(viewPos.getX(), viewPos.getY(), viewPos.getZ(), 1.0f);
        if (std::abs(clipPos.getW()) < 1e-6f) {
            return false;
        }

        const float ndcX = clipPos.getX() / clipPos.getW();
        const float ndcY = clipPos.getY() / clipPos.getW();

        outX = (ndcX * 0.5f + 0.5f) * static_cast<float>(_width);
        outY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(_height);
        return true;
    }

    bool Picker::buildRay(const int x, const int y, Vector3& outOrigin, Vector3& outDirection) const
    {
        if (!_camera || !_camera->camera() || !_camera->entity()) {
            return false;
        }

        const Rect rect = sanitizeRect(x, y, 1, 1);
        const float px = static_cast<float>(rect.x);
        const float py = static_cast<float>(rect.y);

        const float ndcX = (px / static_cast<float>(_width)) * 2.0f - 1.0f;
        const float ndcY = 1.0f - (py / static_cast<float>(_height)) * 2.0f;

        const Matrix4 viewMatrix = _camera->entity()->worldTransform().inverse();
        const Matrix4 viewProjection = _camera->camera()->projectionMatrix() * viewMatrix;
        const Matrix4 invViewProjection = viewProjection.inverse();

        Vector4 nearClip(ndcX, ndcY, 1.0f, 1.0f);
        Vector4 farClip(ndcX, ndcY, 0.0f, 1.0f);

        nearClip = invViewProjection * nearClip;
        farClip = invViewProjection * farClip;

        if (std::abs(nearClip.getW()) < 1e-6f || std::abs(farClip.getW()) < 1e-6f) {
            return false;
        }

        const Vector3 nearWorld(
            nearClip.getX() / nearClip.getW(),
            nearClip.getY() / nearClip.getW(),
            nearClip.getZ() / nearClip.getW()
        );
        const Vector3 farWorld(
            farClip.getX() / farClip.getW(),
            farClip.getY() / farClip.getW(),
            farClip.getZ() / farClip.getW()
        );

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
