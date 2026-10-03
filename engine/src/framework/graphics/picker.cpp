// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 Arnis Lektauers
//
// Created by Arnis Lektauers on 21.03.2026
//
// See picker.h: an id buffer rendered with the pick variant and read back, and a bounds
// fallback for a device that cannot.
//
#include "picker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "core/math/color.h"
#include "core/math/matrix4.h"
#include "core/math/vector4.h"
#include "core/shape/ray.h"
#include "framework/components/camera/cameraComponent.h"
#include "framework/components/render/renderComponent.h"
#include "framework/engine.h"
#include "framework/entity.h"
#include "platform/graphics/graphicsDevice.h"
#include "platform/graphics/renderPass.h"
#include "platform/graphics/renderTarget.h"
#include "platform/graphics/texture.h"
#include "scene/camera.h"
#include "scene/layer.h"
#include "scene/materials/material.h"
#include "scene/materials/standardMaterial.h"
#include "scene/meshInstance.h"
#include "scene/morphInstance.h"
#include "scene/renderer/forwardRenderer.h"

namespace visutwin::canvas
{
    namespace
    {
        // The private layer the id pass draws. No component names it, so the cull sweep
        // finds only the clones the picker put on it.
        constexpr int kPickLayerId = 0x7FFF0001;

        // 24 bits of id in rgb, 0 = nothing: exact through an RGBA8 target, since a
        // value k / 255 stores back as k.
        Color idColor(const uint32_t id)
        {
            return Color(static_cast<float>(id & 0xFFu) / 255.0f,
                static_cast<float>((id >> 8) & 0xFFu) / 255.0f,
                static_cast<float>((id >> 16) & 0xFFu) / 255.0f, 1.0f);
        }
    }

    Picker::Picker(Engine* app, const int width, const int height, const bool depth)
        : _app(app), _depth(depth)
    {
        resize(width, height);
    }

    Picker::~Picker()
    {
        if (_app && _app->renderer() && _pickLayer && _camera && _camera->camera()) {
            _app->renderer()->invalidateCulledInstances(_camera->camera(), _pickLayer.get());
        }
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
        _idBufferValid = false;

        if (!_camera || !_camera->camera() || !_camera->entity()) {
            return;
        }

        collectCandidates();
        _idBufferValid = renderIdBuffer();
    }

    void Picker::collectCandidates()
    {
        const Vector3 cameraPos = _camera->entity()->position();

        for (auto* renderComponent : RenderComponent::instances()) {
            if (!renderComponent || !renderComponent->active()) {
                continue;
            }

            if (!isLayerAllowed(renderComponent->layers())) {
                continue;
            }

            for (auto* meshInstance : renderComponent->meshInstances()) {
                // A hidden instance (one merged into a batch, or hidden by the app) is
                // not drawn, so it cannot be picked: the forward cull skips it as well.
                if (!meshInstance || !meshInstance->node() || !meshInstance->visible()) {
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

    void Picker::ensureTargets()
    {
        auto device = _app ? _app->graphicsDevice() : nullptr;
        if (!device) {
            return;
        }
        if (_colorBuffer && static_cast<int>(_colorBuffer->width()) == _width &&
            static_cast<int>(_colorBuffer->height()) == _height) {
            return;
        }

        TextureOptions colorOptions;
        colorOptions.name = "PickerColor";
        colorOptions.width = static_cast<uint32_t>(_width);
        colorOptions.height = static_cast<uint32_t>(_height);
        colorOptions.format = PixelFormat::PIXELFORMAT_RGBA8;
        colorOptions.mipmaps = false;
        colorOptions.minFilter = FilterMode::FILTER_NEAREST;
        colorOptions.magFilter = FilterMode::FILTER_NEAREST;
        _colorBuffer = std::make_shared<Texture>(device.get(), colorOptions);

        TextureOptions depthOptions = colorOptions;
        depthOptions.name = "PickerDepth";
        depthOptions.format = PixelFormat::PIXELFORMAT_DEPTH;
        _depthBuffer = std::make_shared<Texture>(device.get(), depthOptions);

        RenderTargetOptions targetOptions;
        targetOptions.graphicsDevice = device.get();
        targetOptions.colorBuffer = _colorBuffer.get();
        targetOptions.depthBuffer = _depthBuffer.get();
        targetOptions.depth = true;
        targetOptions.samples = 1;
        targetOptions.name = "PickerTarget";
        _renderTarget = device->createRenderTarget(targetOptions);
    }

    Picker::PickDraw& Picker::pickDrawFor(MeshInstance* source, const uint32_t id)
    {
        PickDraw& draw = _pickDraws[source];
        Material* sourceMaterial = source->material();
        const uint64_t sourceVersion = sourceMaterial ? sourceMaterial->uniformsVersion() : 0;
        const bool stale = !draw.clone || draw.mesh != source->mesh() || draw.node != source->node() ||
            draw.sourceMaterial != sourceMaterial || draw.sourceVersion != sourceVersion;
        if (stale) {
            // The source's material, cloned, so its alpha test, dither, cull mode and
            // textures carry over; drawn opaque, with no blending, so a transparent surface picks like an opaque one.
            draw.material = sourceMaterial ? sourceMaterial->clone() : std::make_shared<StandardMaterial>();
            if (draw.material->alphaMode() == AlphaMode::BLEND) {
                draw.material->setAlphaMode(AlphaMode::OPAQUE);
            }
            draw.clone = source->cloneFor(source->node());
            draw.clone->setMaterial(draw.material);
            draw.mesh = source->mesh();
            draw.node = source->node();
            draw.sourceMaterial = sourceMaterial;
            draw.sourceVersion = sourceVersion;
            draw.id = 0;
        }
        if (draw.id != id) {
            draw.material->setPick(true, idColor(id));
            draw.id = id;
        }
        // A morph's weights are per-instance state the clone copied once; keep them current.
        if (const MorphInstance* morph = source->morphInstance()) {
            if (MorphInstance* cloneMorph = draw.clone->morphInstance()) {
                for (int i = 0; i < morph->weightCount() && i < cloneMorph->weightCount(); ++i) {
                    cloneMorph->setWeight(i, morph->weight(i));
                }
            }
        }
        draw.used = true;
        return draw;
    }

    bool Picker::renderIdBuffer()
    {
        auto device = _app ? _app->graphicsDevice() : nullptr;
        auto renderer = _app ? _app->renderer() : nullptr;
        if (!device || !renderer) {
            return false;
        }
        ensureTargets();
        if (!_renderTarget) {
            return false;
        }

        // One id per candidate, a stand-in drawn for each.
        for (auto& [source, draw] : _pickDraws) {
            draw.used = false;
        }
        _idToMeshInstance.clear();
        std::vector<MeshInstance*> clones;
        clones.reserve(_candidates.size());
        for (const auto& candidate : _candidates) {
            const auto id = static_cast<uint32_t>(_idToMeshInstance.size() + 1);
            if (id > 0xFFFFFFu) {
                break;
            }
            clones.push_back(pickDrawFor(candidate.meshInstance, id).clone.get());
            _idToMeshInstance.push_back(candidate.meshInstance);
        }
        for (auto it = _pickDraws.begin(); it != _pickDraws.end();) {
            it = it->second.used ? std::next(it) : _pickDraws.erase(it);
        }

        Camera* camera = _camera->camera();
        if (!_pickLayer) {
            _pickLayer = std::make_unique<Layer>("Picker", kPickLayerId);
        }
        _pickLayer->removeMeshInstances(_pickLayer->meshInstances());
        _pickLayer->addMeshInstances(clones);
        renderer->invalidateCulledInstances(camera, _pickLayer.get());

        RenderPass pass(device);
        pass.init(_renderTarget);
        const Color clearColor(0.0f, 0.0f, 0.0f, 0.0f);
        const float clearDepth = 1.0f;
        pass.setClearColor(&clearColor);
        pass.setClearDepth(&clearDepth);
        for (const auto& ops : pass.colorArrayOps()) {
            ops->store = true;
        }
        if (pass.depthStencilOps()) {
            pass.depthStencilOps()->storeDepth = true;
        }

        device->beginOfflineWork();
        device->startRenderPass(&pass);
        renderer->renderForwardLayer(camera, _renderTarget.get(), _pickLayer.get(), false);
        device->endRenderPass(&pass);
        device->endOfflineWork();

        std::vector<uint8_t> pixels;
        const size_t pixelCount = static_cast<size_t>(_width) * static_cast<size_t>(_height);
        if (!_colorBuffer->read(pixels) || pixels.size() < pixelCount * 4) {
            return false;
        }
        _idPixels = std::move(pixels);

        _depthPixels.clear();
        if (_depth) {
            std::vector<uint8_t> depthBytes;
            if (_depthBuffer->read(depthBytes) && depthBytes.size() >= pixelCount * sizeof(float)) {
                _depthPixels.resize(pixelCount);
                std::memcpy(_depthPixels.data(), depthBytes.data(), pixelCount * sizeof(float));
            } else {
                spdlog::warn("Picker: the depth buffer could not be read back; getWorldPoint has no answer");
            }
        }

        const Matrix4 view = _camera->entity()->worldTransform().inverse();
        _inverseViewProjection = (camera->projectionMatrix() * view).inverse();
        return true;
    }

    std::vector<MeshInstance*> Picker::getSelection(const int x, const int y, const int width, const int height) const
    {
        if (!_camera) {
            return {};
        }
        const Rect rect = sanitizeRect(x, y, width, height);
        if (!_idBufferValid) {
            return boundsSelection(rect);
        }

        // Every id under the rect, in the order the rows meet them.
        std::vector<MeshInstance*> selection;
        std::unordered_set<uint32_t> seen;
        for (int py = rect.y; py < rect.y + rect.height; ++py) {
            for (int px = rect.x; px < rect.x + rect.width; ++px) {
                const size_t i = (static_cast<size_t>(py) * static_cast<size_t>(_width) + static_cast<size_t>(px)) * 4;
                const uint32_t id = static_cast<uint32_t>(_idPixels[i]) |
                    (static_cast<uint32_t>(_idPixels[i + 1]) << 8) | (static_cast<uint32_t>(_idPixels[i + 2]) << 16);
                if (id == 0 || id > _idToMeshInstance.size() || !seen.insert(id).second) {
                    continue;
                }
                selection.push_back(_idToMeshInstance[id - 1]);
            }
        }
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
        if (!_idBufferValid) {
            return boundsWorldPoint(x, y);
        }
        if (_depthPixels.empty()) {
            return std::nullopt;
        }

        // The depth under the pixel, unprojected through its CENTRE. A pixel
        // at the far plane shows nothing.
        const Rect rect = sanitizeRect(x, y, 1, 1);
        const float depth = _depthPixels[static_cast<size_t>(rect.y) * static_cast<size_t>(_width) +
            static_cast<size_t>(rect.x)];
        if (!(depth < 1.0f)) {
            return std::nullopt;
        }
        const float ndcX = (static_cast<float>(rect.x) + 0.5f) / static_cast<float>(_width) * 2.0f - 1.0f;
        const float ndcY = 1.0f - (static_cast<float>(rect.y) + 0.5f) / static_cast<float>(_height) * 2.0f;
        // Window depth is the GL-style NDC z remapped to [0, 1] on both backends.
        const Vector4 world = _inverseViewProjection * Vector4(ndcX, ndcY, depth * 2.0f - 1.0f, 1.0f);
        if (std::abs(world.getW()) < 1e-12f) {
            return std::nullopt;
        }
        return world.perspectiveDivide();
    }

    // The bounds fallback: every candidate whose projected bounding box overlaps the
    // rect, nearest first.
    std::vector<MeshInstance*> Picker::boundsSelection(const Rect& rect) const
    {
        std::vector<MeshInstance*> selection;
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

    // The bounds fallback (DEVIATION, see picker.h): the pixel's ray against each
    // candidate's bounding SPHERE, so the point is on that sphere, not on the mesh.
    std::optional<Vector3> Picker::boundsWorldPoint(const int x, const int y) const
    {
        Vector3 rayOrigin;
        Vector3 rayDirection;
        if (!buildRay(x, y, rayOrigin, rayDirection)) {
            return std::nullopt;
        }

        const auto selected = boundsSelection(sanitizeRect(x, y, 1, 1));
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

        // Through the CENTRE of the pixel the pick reads, after
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
